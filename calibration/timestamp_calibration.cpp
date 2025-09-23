#include <iostream>
#include <chrono>
#include <vector>
#include <algorithm>
#include <iomanip>
#include <fstream>
#include <cmath>
#include <thread>

/**
 * Timestamp Calibration Program
 *
 * This program calibrates the mapping between OS metrics timestamps (system_clock)
 * and GPU metrics timestamps (steady_clock) to enable accurate correlation.
 *
 * Key differences:
 * - OS metrics: system_clock (wall-clock time, affected by NTP adjustments)
 * - GPU metrics: steady_clock (monotonic time, not affected by clock adjustments)
 */

struct CalibrationPoint {
    std::chrono::system_clock::time_point system_time;
    std::chrono::steady_clock::time_point steady_time;
    double system_seconds;
    double steady_seconds;
};

class TimestampCalibrator {
private:
    std::vector<CalibrationPoint> calibration_points_;
    double offset_seconds_ = 0.0;
    double drift_ppm_ = 0.0;  // Parts per million drift
    bool calibrated_ = false;

public:
    void addCalibrationPoint() {
        auto system_now = std::chrono::system_clock::now();
        auto steady_now = std::chrono::steady_clock::now();

        auto system_seconds = std::chrono::duration<double>(system_now.time_since_epoch()).count();
        auto steady_seconds = std::chrono::duration<double>(steady_now.time_since_epoch()).count();

        calibration_points_.push_back({system_now, steady_now, system_seconds, steady_seconds});

        std::cout << "Calibration point " << calibration_points_.size()
                  << ": System=" << std::fixed << std::setprecision(9) << system_seconds
                  << ", Steady=" << steady_seconds << std::endl;
    }

    bool calibrate() {
        if (calibration_points_.size() < 2) {
            std::cerr << "Need at least 2 calibration points" << std::endl;
            return false;
        }

        // Calculate offset and drift using linear regression
        double sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0;
        int n = calibration_points_.size();

        for (const auto& point : calibration_points_) {
            sum_x += point.steady_seconds;
            sum_y += point.system_seconds;
            sum_xy += point.steady_seconds * point.system_seconds;
            sum_x2 += point.steady_seconds * point.steady_seconds;
        }

        // Linear regression: system_time = offset + (1 + drift) * steady_time
        double slope = (n * sum_xy - sum_x * sum_y) / (n * sum_x2 - sum_x * sum_x);
        offset_seconds_ = (sum_y - slope * sum_x) / n;
        drift_ppm_ = (slope - 1.0) * 1e6;  // Convert to parts per million

        calibrated_ = true;

        std::cout << "\n=== Calibration Results ===" << std::endl;
        std::cout << "Offset: " << std::fixed << std::setprecision(9) << offset_seconds_ << " seconds" << std::endl;
        std::cout << "Drift: " << std::fixed << std::setprecision(3) << drift_ppm_ << " ppm" << std::endl;
        std::cout << "Formula: system_time = " << offset_seconds_ << " + (1 + "
                  << drift_ppm_/1e6 << ") * steady_time" << std::endl;

        // Calculate accuracy
        calculateAccuracy();

        return true;
    }

    double steadyToSystem(double steady_seconds) const {
        if (!calibrated_) {
            std::cerr << "Not calibrated yet!" << std::endl;
            return 0.0;
        }
        return offset_seconds_ + (1.0 + drift_ppm_/1e6) * steady_seconds;
    }

    double systemToSteady(double system_seconds) const {
        if (!calibrated_) {
            std::cerr << "Not calibrated yet!" << std::endl;
            return 0.0;
        }
        return (system_seconds - offset_seconds_) / (1.0 + drift_ppm_/1e6);
    }

    void calculateAccuracy() {
        if (calibration_points_.size() < 2) return;

        double max_error = 0.0;
        double total_error = 0.0;

        for (const auto& point : calibration_points_) {
            double predicted = steadyToSystem(point.steady_seconds);
            double error = std::abs(predicted - point.system_seconds);
            max_error = std::max(max_error, error);
            total_error += error;
        }

        double avg_error = total_error / calibration_points_.size();

        std::cout << "\n=== Accuracy Analysis ===" << std::endl;
        std::cout << "Average error: " << std::fixed << std::setprecision(6) << avg_error << " seconds" << std::endl;
        std::cout << "Maximum error: " << max_error << " seconds" << std::endl;
        std::cout << "Average error: " << std::fixed << std::setprecision(3) << (avg_error * 1000) << " ms" << std::endl;
        std::cout << "Maximum error: " << (max_error * 1000) << " ms" << std::endl;
    }

    void saveCalibration(const std::string& filename) {
        if (!calibrated_) {
            std::cerr << "Not calibrated yet!" << std::endl;
            return;
        }

        std::ofstream file(filename);
        if (!file) {
            std::cerr << "Failed to open " << filename << " for writing" << std::endl;
            return;
        }

        file << "# Timestamp Calibration Data" << std::endl;
        file << "# Generated at: " << std::chrono::system_clock::now().time_since_epoch().count() << std::endl;
        file << "offset_seconds," << std::fixed << std::setprecision(15) << offset_seconds_ << std::endl;
        file << "drift_ppm," << std::fixed << std::setprecision(6) << drift_ppm_ << std::endl;
        file << "calibration_points," << calibration_points_.size() << std::endl;

        file << "\n# Calibration points (system_seconds,steady_seconds)" << std::endl;
        for (const auto& point : calibration_points_) {
            file << point.system_seconds << "," << point.steady_seconds << std::endl;
        }

        std::cout << "Calibration saved to " << filename << std::endl;
    }

    bool loadCalibration(const std::string& filename) {
        std::ifstream file(filename);
        if (!file) {
            std::cerr << "Failed to open " << filename << " for reading" << std::endl;
            return false;
        }

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;

            size_t comma_pos = line.find(',');
            if (comma_pos == std::string::npos) continue;

            std::string key = line.substr(0, comma_pos);
            std::string value = line.substr(comma_pos + 1);

            if (key == "offset_seconds") {
                offset_seconds_ = std::stod(value);
            } else if (key == "drift_ppm") {
                drift_ppm_ = std::stod(value);
            }
        }

        calibrated_ = true;
        std::cout << "Calibration loaded from " << filename << std::endl;
        std::cout << "Offset: " << std::fixed << std::setprecision(9) << offset_seconds_ << " seconds" << std::endl;
        std::cout << "Drift: " << std::fixed << std::setprecision(3) << drift_ppm_ << " ppm" << std::endl;

        return true;
    }

    void analyzeTrustworthiness() {
        if (!calibrated_) {
            std::cerr << "Not calibrated yet!" << std::endl;
            return;
        }

        std::cout << "\n=== Trustworthiness Analysis ===" << std::endl;

        // Check if calibration is recent
        auto now = std::chrono::system_clock::now();
        auto calibration_time = calibration_points_.back().system_time;
        auto age = std::chrono::duration_cast<std::chrono::hours>(now - calibration_time);

        std::cout << "Calibration age: " << age.count() << " hours" << std::endl;

        // Estimate drift over time
        double estimated_drift_seconds = (drift_ppm_ / 1e6) * age.count() * 3600;
        std::cout << "Estimated drift since calibration: " << std::fixed << std::setprecision(6)
                  << estimated_drift_seconds << " seconds" << std::endl;

        // Trustworthiness assessment
        if (age.count() < 1) {
            std::cout << "✓ Calibration is very recent (< 1 hour) - HIGH TRUST" << std::endl;
        } else if (age.count() < 24) {
            std::cout << "✓ Calibration is recent (< 24 hours) - MEDIUM TRUST" << std::endl;
            if (std::abs(estimated_drift_seconds) > 0.001) {
                std::cout << " Warning: Significant drift expected (" << (estimated_drift_seconds * 1000) << " ms)" << std::endl;
            }
        } else {
            std::cout << " Calibration is old (> 24 hours) - LOW TRUST" << std::endl;
            std::cout << " Significant drift expected (" << (estimated_drift_seconds * 1000) << " ms)" << std::endl;
            std::cout << " Consider re-calibrating for accurate results" << std::endl;
        }

        // System clock stability check
        if (std::abs(drift_ppm_) > 100) {
            std::cout << " High drift detected (" << drift_ppm_ << " ppm) - system clock may be unstable" << std::endl;
        } else {
            std::cout << "✓ Low drift detected (" << drift_ppm_ << " ppm) - system clock appears stable" << std::endl;
        }
    }
};

int main() {
    std::cout << "=== Timestamp Calibration Program ===" << std::endl;
    std::cout << "This program calibrates mapping between system_clock and steady_clock" << std::endl;
    std::cout << "OS metrics use system_clock (wall-clock time)" << std::endl;
    std::cout << "GPU metrics use steady_clock (monotonic time)" << std::endl;
    std::cout << std::endl;

    TimestampCalibrator calibrator;

    // Check if calibration file exists
    std::string calibration_file = "timestamp_calibration.csv";
    std::ifstream test_file(calibration_file);
    if (test_file.good()) {
        std::cout << "Found existing calibration file. Loading..." << std::endl;
        if (calibrator.loadCalibration(calibration_file)) {
            calibrator.analyzeTrustworthiness();
            return 0;
        }
    }

    // Perform new calibration
    std::cout << "Performing new calibration..." << std::endl;
    std::cout << "Taking calibration points every 5 seconds for 30 seconds..." << std::endl;
    std::cout << "Press Ctrl+C to stop early" << std::endl;
    std::cout << std::endl;

    for (int i = 0; i < 7; ++i) {  // 7 points over 30 seconds
        calibrator.addCalibrationPoint();
        if (i < 6) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }

    if (calibrator.calibrate()) {
        calibrator.saveCalibration(calibration_file);
        calibrator.analyzeTrustworthiness();

        // Demonstrate conversion
        std::cout << "\n=== Conversion Example ===" << std::endl;
        auto now_steady = std::chrono::steady_clock::now();
        double steady_seconds = std::chrono::duration<double>(now_steady.time_since_epoch()).count();
        double predicted_system = calibrator.steadyToSystem(steady_seconds);
        auto actual_system = std::chrono::system_clock::now();
        double actual_system_seconds = std::chrono::duration<double>(actual_system.time_since_epoch()).count();

        std::cout << "Current steady_clock: " << std::fixed << std::setprecision(9) << steady_seconds << std::endl;
        std::cout << "Predicted system_clock: " << predicted_system << std::endl;
        std::cout << "Actual system_clock: " << actual_system_seconds << std::endl;
        std::cout << "Error: " << std::abs(predicted_system - actual_system_seconds) * 1000 << " ms" << std::endl;
    }

    return 0;
}
