/*
 * QuickRank - A C++ suite of Learning to Rank algorithms
 * Webpage: http://quickrank.isti.cnr.it/
 * Contact: quickrank@isti.cnr.it
 *
 * Unless explicitly acquired and licensed from Licensor under another
 * license, the contents of this file are subject to the Reciprocal Public
 * License ("RPL") Version 1.5, or subsequent versions as allowed by the RPL,
 * and You may not copy or use this file in either source code or executable
 * form, except in compliance with the terms and conditions of the RPL.
 *
 * All software distributed under the RPL is provided strictly on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESS OR IMPLIED, AND
 * LICENSOR HEREBY DISCLAIMS ALL SUCH WARRANTIES, INCLUDING WITHOUT
 * LIMITATION, ANY WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE, QUIET ENJOYMENT, OR NON-INFRINGEMENT. See the RPL for specific
 * language governing rights and limitations under the RPL.
 *
 * Contributor:
 *   HPC. Laboratory - ISTI - CNR - http://hpc.isti.cnr.it/
 */
#include "learning/forests/lambdamartsampling.h"

#include <fstream>
#include <iomanip>
#include <chrono>
#include <random>

namespace quickrank {
namespace learning {
namespace forests {

const std::string LambdaMartSampling::NAME_ = "LAMBDAMART-SAMPLING";


void
LambdaMartSampling::init(
    std::shared_ptr<quickrank::data::VerticalDataset> training_dataset) {
  LambdaMart::init(training_dataset);
}

void LambdaMartSampling::clear(size_t num_features) {
  LambdaMart::clear(num_features);
}

void LambdaMartSampling::learn(std::shared_ptr<quickrank::data::Dataset> training_dataset,
                 std::shared_ptr<quickrank::data::Dataset> validation_dataset,
                 std::shared_ptr<quickrank::metric::ir::Metric> scorer,
                 size_t partial_save, const std::string output_basename) {
  // ---------- Initialization ----------
  std::cout << "# Initialization";
  std::cout.flush();

  std::chrono::high_resolution_clock::time_point chrono_init_start =
      std::chrono::high_resolution_clock::now();

  // create a copy of the training datasets and put it in vertical format
  std::shared_ptr<quickrank::data::VerticalDataset> vertical_training(
      new quickrank::data::VerticalDataset(training_dataset));

  best_metric_on_validation_ = std::numeric_limits<double>::lowest();
  best_metric_on_training_ = std::numeric_limits<double>::lowest();
  best_model_ = 0;

  ensemble_model_.set_capacity(ntrees_);

  init(vertical_training);

  if (validation_dataset) {
    scores_on_validation_ = new Score[validation_dataset->num_instances()]();
  }

  // if the ensemble size is greater than zero, it means the learn method has
  // to start not from scratch but from a previously saved (intermediate) model
  if (ensemble_model_.is_notempty()) {
    best_model_ = ensemble_model_.get_size() - 1;

    // Update the model's outputs on all training samples
    score_dataset(training_dataset, scores_on_training_);
    // run metric
    best_metric_on_training_ = scorer->evaluate_dataset(
        vertical_training, scores_on_training_);

    if (validation_dataset) {
      // Update the model's outputs on all validation samples
      score_dataset(validation_dataset, scores_on_validation_);
      // run metric
      best_metric_on_validation_ = scorer->evaluate_dataset(
          validation_dataset, scores_on_validation_);
    }
  }

  auto chrono_init_end = std::chrono::high_resolution_clock::now();
  double init_time = std::chrono::duration_cast<std::chrono::duration<double>>(
      chrono_init_end - chrono_init_start).count();
  std::cout << ": " << std::setprecision(2) << init_time << " s." << std::endl;

  // ---------- Training ----------
  std::cout << std::fixed << std::setprecision(4);

  std::cout << "# Training:" << std::endl;
  std::cout << "# -------------------------" << std::endl;
  std::cout << "# iter. training validation" << std::endl;
  std::cout << "# -------------------------" << std::endl;

  // shows the performance of the already trained model..
  if (ensemble_model_.is_notempty()) {
    std::cout << std::setw(7) << ensemble_model_.get_size()
              << std::setw(9) << best_metric_on_training_;

    if (validation_dataset)
      std::cout << std::setw(9) << best_metric_on_validation_;

    std::cout << " *" << std::endl;
  }

  auto chrono_train_start = std::chrono::high_resolution_clock::now();

  // Used for document sampling and node splitting
  size_t nsampleids = training_dataset->num_instances();
  size_t *sampleids = new size_t[nsampleids];

  size_t step_size_sampling = 0;
  if (sampling_iterations > 0 && max_sampling_factor > 0) {
    step_size_sampling = (size_t) std::round(
      max_sampling_factor * nsampleids / (
          (float) ntrees_ / sampling_iterations));
  }

  // If we do not use document sampling, we fill the sampleids only once
  #pragma omp parallel for
  for (size_t i = 0; i < nsampleids; ++i)
    sampleids[i] = i;

//  if (subsample_ > 1.0f) {
//    // >1: Max feature is the number of features to use
//    nsampleids = (size_t) subsample_;
//  } else if (subsample_ < 1.0f) {
//    // <1: Max feature is the fraction of features to use
//    nsampleids = (size_t) std::ceil(subsample_ * nsampleids);
//  }

  // start iterations from 0 or (ensemble_size - 1)
  for (size_t m = ensemble_model_.get_size(); m < ntrees_; ++m) {
    if (validation_dataset
        && (valid_iterations_ && m > best_model_ + valid_iterations_))
      break;

    compute_pseudoresponses(vertical_training, scorer.get());

    if (step_size_sampling > 0 && m > 0 && m % sampling_iterations == 0) {

      std::sort(&sampleids[0], &sampleids[nsampleids],
                [this, &training_dataset](size_t i1, size_t i2) {
                  return
                      (training_dataset->getLabel(i1) > 0 ||
                      training_dataset->getLabel(i2) == 0) &&
                      scores_on_training_[i1] > scores_on_training_[i2];
                });

      nsampleids -= step_size_sampling;

      std::cout << "Reducing training size from "
                << nsampleids + step_size_sampling << " to "
                << nsampleids << std::endl;
    }

    size_t nsampleids_iter = nsampleids;
    if (subsample_ != 1.0f) {

      if (subsample_ > 1.0f) {
        // >1: Max feature is the number of features to use
        nsampleids_iter = (size_t) std::min((size_t) subsample_, nsampleids);
      } else {
        // <1: Max feature is the fraction of features to use
        nsampleids_iter = (size_t) std::ceil(subsample_ * nsampleids);
      }

      // shuffle the sample idx
      auto seed = std::chrono::system_clock::now().time_since_epoch().count();
      auto rng = std::default_random_engine(seed);
      std::shuffle(&sampleids[0],
                   &sampleids[nsampleids],
                   rng);
    }

    // update the histogram with these training_setting labels
    // (the feature histogram will be used to find the best tree rtnode)
    hist_->update(pseudoresponses_, nsampleids_iter, sampleids);

    // Fit a regression tree
    std::unique_ptr<RegressionTree> tree =
        fit_regressor_on_gradient(vertical_training, sampleids);

    //add this tree to the ensemble (our model)
    ensemble_model_.push(tree->get_proot(), shrinkage_, 0);  // maxlabel);

    //Update the model's outputs on all training samples
    update_modelscores(vertical_training, scores_on_training_, tree.get());
    // run metric
    quickrank::MetricScore metric_on_training = scorer->evaluate_dataset(
        vertical_training, scores_on_training_);

    //show results
    std::cout << std::setw(7) << m + 1 << std::setw(9) << metric_on_training;

    //Evaluate the current model on the validation data (if available)
    if (validation_dataset) {
      // update validation scores
      update_modelscores(validation_dataset, scores_on_validation_, tree.get());

      // run metric
      quickrank::MetricScore metric_on_validation = scorer->evaluate_dataset(
          validation_dataset, scores_on_validation_);
      std::cout << std::setw(9) << metric_on_validation;

      if (metric_on_validation > best_metric_on_validation_) {
        best_metric_on_training_ = metric_on_training;
        best_metric_on_validation_ = metric_on_validation;
        best_model_ = ensemble_model_.get_size() - 1;
        std::cout << " *";
      }
    } else {
      if (metric_on_training > best_metric_on_training_) {
        best_metric_on_training_ = metric_on_training;
        best_model_ = ensemble_model_.get_size() - 1;
        std::cout << " *";
      }
    }
    std::cout << std::endl;

    if (partial_save != 0 and !output_basename.empty()
        and (m + 1) % partial_save == 0) {
      save(output_basename, m + 1);
    }

  }

  delete(sampleids);

  //Rollback to the best model observed on the validation data
  if (validation_dataset) {
    while (ensemble_model_.is_notempty()
        && ensemble_model_.get_size() > best_model_ + 1) {
      ensemble_model_.pop();
    }
  }

  auto chrono_train_end = std::chrono::high_resolution_clock::now();
  double train_time = std::chrono::duration_cast<std::chrono::duration<double>>(
      chrono_train_end - chrono_train_start).count();

  //Finishing up
  std::cout << std::endl;
  std::cout << *scorer << " on training data = " << best_metric_on_training_
            << std::endl;

  if (validation_dataset) {
    std::cout << *scorer << " on validation data = "
              << best_metric_on_validation_ << std::endl;
  }

  clear(vertical_training->num_features());

  std::cout << std::endl;
  std::cout << "#\t Training Time: " << std::setprecision(2) << train_time
            << " s." << std::endl;
}

std::ostream &LambdaMartSampling::put(std::ostream &os) const {
  Mart::put(os);
  if (sampling_iterations != 0)
    os << "# sampling iterations = " << sampling_iterations << std::endl;
  if (max_sampling_factor != 0)
    os << "# max sampling factor = " << max_sampling_factor << std::endl;
  return os;
}

}  // namespace forests
}  // namespace learning
}  // namespace quickrank
