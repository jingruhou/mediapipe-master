// Copyright 2019 The MediaPipe Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mediapipe/framework/subgraph.h"

#include <fstream>
#include <iostream>
#include <sstream>

#include "mediapipe/framework/port/ret_check.h"
#include "mediapipe/framework/tool/template_expander.h"

namespace mediapipe {

Subgraph::Subgraph() {}

Subgraph::~Subgraph() {}

ProtoSubgraph::ProtoSubgraph(const CalculatorGraphConfig& config)
    : config_(config) {}

ProtoSubgraph::~ProtoSubgraph() {}

::mediapipe::StatusOr<CalculatorGraphConfig> ProtoSubgraph::GetConfig(
    const Subgraph::SubgraphOptions& options) {
  return config_;
}

TemplateSubgraph::TemplateSubgraph(const CalculatorGraphTemplate& templ)
    : templ_(templ) {}

TemplateSubgraph::~TemplateSubgraph() {}

::mediapipe::StatusOr<CalculatorGraphConfig> TemplateSubgraph::GetConfig(
    const Subgraph::SubgraphOptions& options) {
  const TemplateDict& arguments =
      options.GetExtension(TemplateSubgraphOptions::ext).dict();
  tool::TemplateExpander expander;
  CalculatorGraphConfig config;
  MP_RETURN_IF_ERROR(expander.ExpandTemplates(arguments, templ_, &config));
  return config;
}

GraphRegistry GraphRegistry::global_graph_registry;

GraphRegistry::GraphRegistry()
    : global_factories_(SubgraphRegistry::functions()) {}

GraphRegistry::GraphRegistry(
    FunctionRegistry<std::unique_ptr<Subgraph>>* factories)
    : global_factories_(factories) {}

void GraphRegistry::Register(const std::string& type_name,
                             const CalculatorGraphConfig& config) {
  local_factories_.Register(type_name, [config] {
    auto result = absl::make_unique<ProtoSubgraph>(config);
    return std::unique_ptr<Subgraph>(result.release());
  });
}

void GraphRegistry::Register(const std::string& type_name,
                             const CalculatorGraphTemplate& templ) {
  local_factories_.Register(type_name, [templ] {
    auto result = absl::make_unique<TemplateSubgraph>(templ);
    return std::unique_ptr<Subgraph>(result.release());
  });
}

bool GraphRegistry::IsRegistered(const std::string& ns,
                                 const std::string& type_name) const {
  return local_factories_.IsRegistered(ns, type_name) ||
         global_factories_->IsRegistered(ns, type_name);
}

::mediapipe::StatusOr<CalculatorGraphConfig> GraphRegistry::CreateByName(
    const std::string& ns, const std::string& type_name,
    const Subgraph::SubgraphOptions* options) const {
  Subgraph::SubgraphOptions graph_options;
  if (options) {
    graph_options = *options;
  }
  ::mediapipe::StatusOr<std::unique_ptr<Subgraph>> maker =
      local_factories_.IsRegistered(ns, type_name)
          ? local_factories_.Invoke(ns, type_name)
          : global_factories_->Invoke(ns, type_name);
  MP_RETURN_IF_ERROR(maker.status());
  return maker.ValueOrDie()->GetConfig(graph_options);
}

}  // namespace mediapipe
