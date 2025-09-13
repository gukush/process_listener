// orc2csv.cpp
#include <orc/OrcFile.hh>
#include <orc/Reader.hh>
#include <orc/Vector.hh>

#include <zstd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

static void fail(const std::string& msg) {
  std::cerr << "error: " << msg << "\n";
  std::exit(1);
}

static bool has_suffix(const std::string& s, const std::string& suf) {
  return s.size() >= suf.size() && s.compare(s.size()-suf.size(), suf.size(), suf) == 0;
}

// Minimal streaming ZSTD decompress to a temp file.
// Returns path to the decompressed file; caller should delete it later.
static std::string zstd_decompress_to_temp(const std::string& in_path) {
  std::ifstream fin(in_path, std::ios::binary);
  if (!fin) fail("cannot open input: " + in_path);

  // temp file next to the input
  std::string out_path = in_path;
  if (has_suffix(out_path, ".zst")) out_path.erase(out_path.size()-4);
  else if (has_suffix(out_path, ".zstd")) out_path.erase(out_path.size()-5);
  else out_path += ".unzst";

  std::ofstream fout(out_path, std::ios::binary);
  if (!fout) fail("cannot create temp file: " + out_path);

  const size_t IN_CHUNK = 1 << 20;   // 1 MiB
  const size_t OUT_CHUNK = 1 << 20;

  std::vector<char> inbuf(IN_CHUNK), outbuf(OUT_CHUNK);

  ZSTD_DCtx* dctx = ZSTD_createDCtx();
  if (!dctx) fail("ZSTD_createDCtx failed");

  ZSTD_inBuffer zin{nullptr, 0, 0};
  ZSTD_outBuffer zout{nullptr, 0, 0};

  while (true) {
    if (zin.pos == zin.size) {
      fin.read(inbuf.data(), inbuf.size());
      zin.src = inbuf.data();
      zin.size = static_cast<size_t>(fin.gcount());
      zin.pos = 0;
      if (zin.size == 0) break; // EOF
    }
    zout.dst = outbuf.data();
    zout.size = outbuf.size();
    zout.pos = 0;
    size_t ret = ZSTD_decompressStream(dctx, &zout, &zin);
    if (ZSTD_isError(ret)) {
      ZSTD_freeDCtx(dctx);
      fail(std::string("zstd decompress error: ") + ZSTD_getErrorName(ret));
    }
    fout.write(reinterpret_cast<const char*>(zout.dst), static_cast<std::streamsize>(zout.pos));
    if (!fout) {
      ZSTD_freeDCtx(dctx);
      fail("write failed to temp file");
    }
  }

  ZSTD_freeDCtx(dctx);
  fout.flush();
  if (!fout) fail("flush failed on temp file");
  return out_path;
}

static std::string csv_escape(const std::string& s) {
  bool needs_quotes = false;
  for (char c : s) {
    if (c == ',' || c == '"' || c == '\n' || c == '\r') { needs_quotes = true; break; }
  }
  if (!needs_quotes) return s;
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (char c : s) {
    if (c == '"') out.push_back('"'); // double quotes
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

static std::string timestamp_to_iso(int64_t seconds, int64_t nanos) {
  // Format as epoch millis to keep it simple/stable (avoid TZ issues)
  long double ms = static_cast<long double>(seconds) * 1000.0L + (nanos / 1000000.0L);
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss.precision(3);
  oss << ms; // epoch millis with 3 decimals
  return oss.str();
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0] << " <input.orc|input.orc.zst> <output.csv>\n";
    return 2;
  }
  std::string in_path = argv[1];
  std::string out_csv = argv[2];

  // If the file is externally compressed (.zst/.zstd), decompress it first.
  bool is_zst = has_suffix(in_path, ".zst") || has_suffix(in_path, ".zstd");
  std::string orc_path = in_path;
  std::string temp_path;
  if (is_zst) {
    std::cerr << "Decompressing ZSTD: " << in_path << " ...\n";
    temp_path = zstd_decompress_to_temp(in_path);
    orc_path = temp_path;
  }

  try {
    orc::ReaderOptions ropts;
    std::unique_ptr<orc::Reader> reader = orc::createReader(orc::readLocalFile(orc_path), ropts);
    const orc::Type& rootType = reader->getType();
    if (rootType.getKind() != orc::STRUCT) {
      fail("Top-level ORC type is not a struct; unsupported for CSV output");
    }

    // Column names (header)
    std::vector<std::string> col_names;
    col_names.reserve(rootType.getSubtypeCount());
    for (uint64_t i = 0; i < rootType.getSubtypeCount(); ++i) {
      col_names.emplace_back(rootType.getFieldName(i));
    }

    std::ofstream out(out_csv, std::ios::binary);
    if (!out) fail("cannot open output csv: " + out_csv);

    // Write header
    for (size_t i = 0; i < col_names.size(); ++i) {
      if (i) out << ',';
      out << csv_escape(col_names[i]);
    }
    out << "\n";

    // Prepare row reader
    orc::RowReaderOptions rropts;
    std::unique_ptr<orc::RowReader> rows = reader->createRowReader(rropts);
    std::unique_ptr<orc::ColumnVectorBatch> batch = rows->createRowBatch(1024 * 1024);

    while (rows->next(*batch)) {
      auto* root = dynamic_cast<orc::StructVectorBatch*>(batch.get());
      int64_t n = root->numElements;

      // Grab child batches once
      std::vector<orc::ColumnVectorBatch*> cols;
      cols.reserve(rootType.getSubtypeCount());
      for (uint64_t i = 0; i < rootType.getSubtypeCount(); ++i) {
        cols.push_back(root->fields[i]);
      }

      for (int64_t r = 0; r < n; ++r) {
        for (size_t c = 0; c < cols.size(); ++c) {
          if (c) out << ',';

          const orc::Type& t = *rootType.getSubtype(static_cast<uint64_t>(c));
          orc::ColumnVectorBatch* vb = cols[c];

          // Null check
          bool is_null = (vb->hasNulls && !vb->notNull[r]);
          if (is_null) { /* leave empty */ }
          else {
            switch (t.getKind()) {
              case orc::BOOLEAN: {
                auto* b = dynamic_cast<orc::LongVectorBatch*>(vb);
                out << (b->data[r] ? "true" : "false");
                break;
              }
              case orc::BYTE:
              case orc::SHORT:
              case orc::INT:
              case orc::LONG: {
                auto* v = dynamic_cast<orc::LongVectorBatch*>(vb);
                out << v->data[r];
                break;
              }
              case orc::FLOAT: {
                auto* v = dynamic_cast<orc::FloatVectorBatch*>(vb);
                out << v->data[r];
                break;
              }
              case orc::DOUBLE: {
                auto* v = dynamic_cast<orc::DoubleVectorBatch*>(vb);
                out << v->data[r];
                break;
              }
              case orc::STRING:
              case orc::VARCHAR:
              case orc::CHAR: {
                auto* v = dynamic_cast<orc::StringVectorBatch*>(vb);
                std::string s(v->data[r], static_cast<size_t>(v->length[r]));
                out << csv_escape(s);
                break;
              }
              case orc::TIMESTAMP: {
                auto* v = dynamic_cast<orc::TimestampVectorBatch*>(vb);
                out << timestamp_to_iso(v->data[r], v->nanoseconds[r]);
                break;
              }
              // Add more cases as needed (DATE, DECIMAL, BINARY, etc.)
              default: {
                // Fallback: leave empty for unsupported types
                break;
              }
            }
          }
        }
        out << "\n";
      }
    }

    out.flush();
    if (!out) fail("failed writing CSV");

    // Cleanup temp if created
    if (!temp_path.empty()) {
      std::error_code ec;
      fs::remove(temp_path, ec);
    }

    std::cerr << "Done. Wrote CSV: " << out_csv << "\n";
  } catch (const std::exception& ex) {
    fail(std::string("exception: ") + ex.what());
  }
  return 0;
}
