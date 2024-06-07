#include <iostream>
#include <vector>
#include "ort_genai.h"

#include "engine.h"

OgaEngine::OgaEngine(const char* config_path) {
  // Load the configuration file
  // Initialize the engine

  std::cout << "Creating model..." << std::endl;
  model_ = OgaModel::Create(config_path);
  std::cout << "Creating tokenizer..." << std::endl;
  tokenizer_ = OgaTokenizer::Create(*model_);
}

std::vector<const char*> OgaEngine::Schedule() {
  // Schedule the requests
  // Return the scheduled requests

  std::vector<const char*> scheduled_prompts;
  for (int i = 0; i < kMaxBatchSize; i++) {
    if (unscheduled_prompts_.empty()) {
      break;
    }
    scheduled_prompts.push_back(unscheduled_prompts_.front());
    unscheduled_prompts_.pop();
  }
  return scheduled_prompts;
}

std::vector<std::string> OgaEngine::Generate(std::vector<const char*> prompts) {
  // Generate the output for the scheduled requests
  // Print the output

  for (const char* prompt : prompts) {
    unscheduled_prompts_.push(prompt);
  }

  std::vector<std::string> outputs(prompts.size());

  while (!unscheduled_prompts_.empty()) {
    std::vector<const char*> scheduled_prompts = Schedule();
    auto sequences = OgaSequences::Create();

    tokenizer_->EncodeBatch(scheduled_prompts, *sequences);

    auto params = OgaGeneratorParams::Create(*model_);
    params->SetSearchOption("max_length", 200);
    params->SetInputSequences(*sequences);

    auto output_sequences = model_->Generate(*params);

    auto out_strings = tokenizer_->DecodeBatch(std::move(output_sequences));
    for (auto out_string : out_strings) {
      outputs.push_back(std::string(out_string));
    }
  }
  return outputs;
}

int64_t OgaEngine::AddRequest(const char* prompt) {
  // Add the request to the queue
  // Return the request ID
  int64_t rid = request_id_;
  unscheduled_prompts_.push(prompt);
  request_id_++;
  return rid;
}