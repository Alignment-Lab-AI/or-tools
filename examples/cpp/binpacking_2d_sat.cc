// Copyright 2010-2022 Google LLC
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// This file solves a 2D Bin Packing problem.
// It loads the size of the main rectangle, all available items (rectangles
// too), and tries to fit all rectangles in the minimum numbers of bins (they
// have the size of the main rectangle.)

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/log/check.h"
#include "absl/strings/str_cat.h"
#include "google/protobuf/text_format.h"
#include "ortools/base/init_google.h"
#include "ortools/base/logging.h"
#include "ortools/base/path.h"
#include "ortools/packing/binpacking_2d_parser.h"
#include "ortools/packing/multiple_dimensions_bin_packing.pb.h"
#include "ortools/sat/cp_model.h"
#include "ortools/sat/cp_model.pb.h"
#include "ortools/sat/cp_model_solver.h"
#include "ortools/sat/sat_parameters.pb.h"
#include "ortools/sat/util.h"

ABSL_FLAG(std::string, input, "", "Input file.");
ABSL_FLAG(int, instance, -1, "Instance number if the file.");
ABSL_FLAG(std::string, params, "", "Sat parameters in text proto format.");
ABSL_FLAG(int, max_bins, 0,
          "Maximum number of bins. The 0 default value implies the code will "
          "use some heuristics to compute this number.");
ABSL_FLAG(bool, symmetry_breaking, true, "Use symmetry breaking constraints");
ABSL_FLAG(bool, use_global_cumulative, true,
          "Use a globalcumulative relaxation");

namespace operations_research {
namespace sat {

// Load a 2D bin packing problem and solve it.
void LoadAndSolve(const std::string& file_name, int instance) {
  packing::BinPacking2dParser parser;
  if (!parser.Load2BPFile(file_name, instance)) {
    LOG(FATAL) << "Cannot read instance " << instance << " from file '"
               << file_name << "'";
  }
  packing::MultipleDimensionsBinPackingProblem problem = parser.problem();
  LOG(INFO) << "Successfully loaded instance " << instance << " from file '"
            << file_name << "'";
  LOG(INFO) << "Instance has " << problem.items_size() << " items";

  const auto box_dimensions = problem.box_shape().dimensions();
  const int num_dimensions = box_dimensions.size();
  const int num_items = problem.items_size();

  // Non overlapping.
  if (num_dimensions == 1) {
    LOG(FATAL) << "One dimension is not supported.";
  } else if (num_dimensions != 2) {
    LOG(FATAL) << num_dimensions << " dimensions not supported.";
  }

  const int64_t area_of_one_bin = box_dimensions[0] * box_dimensions[1];
  int64_t sum_of_items_area = 0;
  for (const auto& item : problem.items()) {
    CHECK_EQ(1, item.shapes_size());
    const auto& shape = item.shapes(0);
    CHECK_EQ(2, shape.dimensions_size());
    sum_of_items_area += shape.dimensions(0) * shape.dimensions(1);
  }

  const int64_t trivial_lb = CeilOfRatio(sum_of_items_area, area_of_one_bin);
  LOG(INFO) << "Trivial lower bound of the number of bins = " << trivial_lb;
  const int max_bins = absl::GetFlag(FLAGS_max_bins) == 0
                           ? trivial_lb * 2
                           : absl::GetFlag(FLAGS_max_bins);
  if (absl::GetFlag(FLAGS_max_bins) == 0) {
    LOG(INFO) << "Setting max_bins to " << max_bins;
  }

  CpModelBuilder cp_model;
  cp_model.SetName(absl::StrCat(
      "binpacking_2d_", file::Stem(absl::GetFlag(FLAGS_input)), "_", instance));

  // We do not support multiple shapes per item.
  for (int item = 0; item < num_items; ++item) {
    const int num_shapes = problem.items(item).shapes_size();
    CHECK_EQ(1, num_shapes);
  }

  // Create one Boolean variable per item and per bin.
  std::vector<std::vector<BoolVar>> item_to_bin(num_items);
  for (int item = 0; item < num_items; ++item) {
    item_to_bin[item].resize(max_bins);
    for (int b = 0; b < max_bins; ++b) {
      item_to_bin[item][b] = cp_model.NewBoolVar();
    }
  }

  // Exactly one bin is selected for each item.
  for (int item = 0; item < num_items; ++item) {
    cp_model.AddExactlyOne(item_to_bin[item]);
  }

  // Detect incompatible pairs of items and add conflict at the bin level.
  int num_incompatible_pairs = 0;
  for (int i1 = 0; i1 + 1 < num_items; ++i1) {
    for (int i2 = i1 + 1; i2 < num_items; ++i2) {
      if ((problem.items(i1).shapes(0).dimensions(0) +
               problem.items(i2).shapes(0).dimensions(0) >
           box_dimensions[0]) &&
          (problem.items(i1).shapes(0).dimensions(1) +
               problem.items(i2).shapes(0).dimensions(1) >
           box_dimensions[1])) {
        num_incompatible_pairs++;
        for (int b = 0; b < max_bins; ++b) {
          cp_model.AddAtMostOne({item_to_bin[i1][b], item_to_bin[i2][b]});
        }
      }
    }
  }
  if (num_incompatible_pairs > 0) {
    LOG(INFO) << num_incompatible_pairs << " incompatible pairs of items";
  }

  // Manages positions and sizes for each item.
  std::vector<std::vector<std::vector<IntervalVar>>>
      interval_by_item_bin_dimension(num_items);
  std::vector<std::vector<IntVar>> starts_by_dimension(num_items);
  for (int item = 0; item < num_items; ++item) {
    interval_by_item_bin_dimension[item].resize(max_bins);
    starts_by_dimension[item].resize(num_dimensions);
    for (int b = 0; b < max_bins; ++b) {
      interval_by_item_bin_dimension[item][b].resize(num_dimensions);
      for (int dim = 0; dim < num_dimensions; ++dim) {
        const int64_t dimension = box_dimensions[dim];
        const int64_t size = problem.items(item).shapes(0).dimensions(dim);
        IntVar start;
        if (b == 0) {
          start = cp_model.NewIntVar({0, dimension - size});
          starts_by_dimension[item][dim] = start;
        } else {
          start = starts_by_dimension[item][dim];
        }
        interval_by_item_bin_dimension[item][b][dim] =
            cp_model.NewOptionalFixedSizeIntervalVar(start, size,
                                                     item_to_bin[item][b]);
      }
    }
  }

  // Non overlapping.
  LOG(INFO) << "Box size: " << box_dimensions[0] << "*" << box_dimensions[1];
  for (int b = 0; b < max_bins; ++b) {
    NoOverlap2DConstraint no_overlap_2d = cp_model.AddNoOverlap2D();
    for (int item = 0; item < num_items; ++item) {
      no_overlap_2d.AddRectangle(interval_by_item_bin_dimension[item][b][0],
                                 interval_by_item_bin_dimension[item][b][1]);
    }
  }

  // Objective variable.
  const IntVar obj = cp_model.NewIntVar({trivial_lb, max_bins});

  // Global cumulative.
  if (absl::GetFlag(FLAGS_use_global_cumulative)) {
    DCHECK_EQ(num_dimensions, 2);
    for (int dim = 0; dim < num_dimensions; ++dim) {
      const int other_size = box_dimensions[1 - dim];
      CumulativeConstraint cumul = cp_model.AddCumulative(obj * other_size);
      for (int item = 0; item < num_items; ++item) {
        const int size = problem.items(item).shapes(0).dimensions(dim);
        const int demand = problem.items(item).shapes(0).dimensions(1 - dim);
        cumul.AddDemand(cp_model.NewFixedSizeIntervalVar(
                            starts_by_dimension[item][dim], size),
                        demand);
      }
    }
  }

  // Maintain one Boolean variable per bin that indicates if the bin is used
  // or not.
  std::vector<BoolVar> bin_is_used(max_bins);
  for (int b = 0; b < max_bins; ++b) {
    bin_is_used[b] = cp_model.NewBoolVar();

    // Link bin_is_used[i] with the items in bin i.
    std::vector<BoolVar> all_items_in_bin;
    for (int item = 0; item < num_items; ++item) {
      cp_model.AddImplication(item_to_bin[item][b], bin_is_used[b]);
      all_items_in_bin.push_back(item_to_bin[item][b]);
    }
    cp_model.AddBoolOr(all_items_in_bin).OnlyEnforceIf(bin_is_used[b]);
  }

  // Objective definition.
  cp_model.Minimize(obj);
  for (int b = trivial_lb; b + 1 < max_bins; ++b) {
    cp_model.AddGreaterOrEqual(obj, b + 1).OnlyEnforceIf(bin_is_used[b]);
    cp_model.AddImplication(bin_is_used[b + 1], bin_is_used[b]);
  }

  if (absl::GetFlag(FLAGS_symmetry_breaking)) {
    // Symmetry breaking: item[i] is in bin <= i for the first max_bins items.
    for (int i = 0; i + 1 < std::min(num_items, max_bins); ++i) {
      for (int b = i + 1; b < max_bins; ++b) {
        cp_model.FixVariable(item_to_bin[i][b], false);
      }
    }
  }

  // Setup parameters.
  SatParameters parameters;
  parameters.set_log_search_progress(true);

  // Parse the --params flag.
  if (!absl::GetFlag(FLAGS_params).empty()) {
    CHECK(google::protobuf::TextFormat::MergeFromString(
        absl::GetFlag(FLAGS_params), &parameters))
        << absl::GetFlag(FLAGS_params);
  }

  // If number of workers is >= 16 and < 24, we prefer replacing
  // objective_lb_search by objective_shaving_search.
  if (parameters.num_workers() >= 16 && parameters.num_workers() < 24) {
    parameters.add_ignore_subsolvers("objective_lb_search");
    parameters.add_extra_subsolvers("objective_shaving_search");
  }

  // We rely on the solver default logging to log the number of bins.
  const CpSolverResponse response =
      SolveWithParameters(cp_model.Build(), parameters);
}

}  // namespace sat
}  // namespace operations_research

int main(int argc, char** argv) {
  absl::SetFlag(&FLAGS_stderrthreshold, 0);
  InitGoogle(argv[0], &argc, &argv, true);
  if (absl::GetFlag(FLAGS_input).empty()) {
    LOG(FATAL) << "Please supply a data file with --input=";
  }
  if (absl::GetFlag(FLAGS_instance) == -1) {
    LOG(FATAL) << "Please supply a valid instance number with --instance=";
  }

  operations_research::sat::LoadAndSolve(absl::GetFlag(FLAGS_input),
                                         absl::GetFlag(FLAGS_instance));
  return EXIT_SUCCESS;
}
