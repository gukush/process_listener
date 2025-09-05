// enhanced_message_interceptor.cpp

void EnhancedMessageInterceptor::onTaskInit(const json& msg) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    
    TaskInfo task;
    task.task_id = msg["taskId"];
    task.strategy_id = msg.value("strategyId", "");
    task.init_time = std::chrono::steady_clock::now();
    task.status = "initialized";
    
    tasks_[task.task_id] = task;
    
    // Notify chunk tracker about new task
    chunk_tracker_->onTaskInit(task.task_id);
}

void EnhancedMessageInterceptor::onChunkAssign(const json& msg) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    
    ChunkInfo chunk;
    chunk.chunk_id = msg["chunkId"];
    chunk.task_id = msg["taskId"];
    chunk.replica_id = msg.value("replica", 0);
    chunk.arrival_time = std::chrono::steady_clock::now();
    chunk.start_time = chunk.arrival_time;  // Assume immediate start
    chunk.status = "processing";
    
    // Calculate payload size if available
    if (msg.contains("payload")) {
        chunk.payload_size = msg["payload"].dump().size();
    }
    
    chunks_[chunk.chunk_id] = chunk;
    
    // Update task-chunk mapping
    task_to_chunks_.emplace(chunk.task_id, chunk.chunk_id);
    
    // Update task's chunk list
    auto& task = tasks_[chunk.task_id];
    task.chunk_ids.push_back(chunk.chunk_id);
    task.status = "processing";
    
    // Notify tracker
    chunk_tracker_->onChunkArrival(chunk.chunk_id, chunk.task_id);
}

void EnhancedMessageInterceptor::onChunkResult(const json& msg) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    
    std::string chunk_id = msg["chunkId"];
    auto it = chunks_.find(chunk_id);
    if (it != chunks_.end()) {
        it->second.complete_time = std::chrono::steady_clock::now();
        it->second.status = msg.value("status", "completed");
        
        // Check if all chunks for this task are complete
        std::string task_id = it->second.task_id;
        auto range = task_to_chunks_.equal_range(task_id);
        bool all_complete = true;
        
        for (auto chunk_it = range.first; chunk_it != range.second; ++chunk_it) {
            const auto& chunk = chunks_[chunk_it->second];
            if (chunk.status == "processing" || chunk.status == "assigned") {
                all_complete = false;
                break;
            }
        }
        
        if (all_complete) {
            tasks_[task_id].status = "completed";
            onTaskComplete(task_id);
        }
        
        // Notify tracker
        chunk_tracker_->onChunkComplete(chunk_id, it->second.status);
    }
}

std::vector<std::string> EnhancedMessageInterceptor::getActiveChunksAt(
    std::chrono::steady_clock::time_point time) const {
    
    std::lock_guard<std::mutex> lock(data_mutex_);
    std::vector<std::string> active;
    
    for (const auto& [chunk_id, info] : chunks_) {
        if (info.arrival_time <= time && 
            (info.complete_time == std::chrono::steady_clock::time_point{} ||
             info.complete_time > time)) {
            active.push_back(chunk_id);
        }
    }
    
    return active;
}
