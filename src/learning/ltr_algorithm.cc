/*
 * QuickRank - A C++ suite of Learning to Rank algorithms
 * Webpage: http://quickrank.isti.cnr.it/
 * Contact: quickrank@isti.cnr.it
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Contributor:
 *   HPC. Laboratory - ISTI - CNR - http://hpc.isti.cnr.it/
 */
#include <fstream>

#include <boost/property_tree/xml_parser.hpp>
#include <boost/foreach.hpp>

#include "learning/ltr_algorithm.h"

#include "learning/forests/mart.h"
#include "learning/forests/lambdamart.h"
#include "learning/forests/matrixnet.h"
// Add by Chiara Pierucci Andrea Battistini
#include "learning/linear/coordinate_ascent.h"

#ifdef _OPENMP
#include <omp.h>
#else
#include "utils/omp-stubs.h"
#endif

namespace quickrank {
namespace learning {

void LTR_Algorithm::score_dataset(std::shared_ptr<data::Dataset> dataset,
                                  Score* scores) const {
  preprocess_dataset(dataset);
  for (unsigned int q = 0; q < dataset->num_queries(); q++) {
    std::shared_ptr<data::QueryResults> r = dataset->getQueryResults(q);
    score_query_results(r, scores, dataset->num_instances());
    scores += r->num_results();
  }
}

// assumes vertical dataset
// offset to next feature of the same instance
void LTR_Algorithm::score_query_results(
    std::shared_ptr<data::QueryResults> results, Score* scores,
    unsigned int offset) const {
  const quickrank::Feature* d = results->features();
  for (unsigned int i = 0; i < results->num_results(); i++) {
    scores[i] = score_document(d, offset);
    d++;
  }
}

// assumes vertical dataset
Score LTR_Algorithm::score_document(const quickrank::Feature* d,
                                    const unsigned int offset) const {
  return 0.0;
}

void LTR_Algorithm::save(std::string output_basename, int iteration) const {
  if (!output_basename.empty()) {
    std::string filename(output_basename);
    if (iteration != -1)
      filename += ".T"+std::to_string(iteration) + ".xml";
    std::ofstream output_stream;
    output_stream.open(filename);
    // Wrap actual model
    output_stream << "<ranker>" << std::endl;

    // Save the actual model
    save_model_to_file(output_stream);

    output_stream << "</ranker>" << std::endl;

    output_stream.close();
  }
}

std::shared_ptr<LTR_Algorithm> LTR_Algorithm::load_model_from_file(
    std::string model_filename) {
  if (model_filename.empty()) {
    std::cerr << "!!! Model filename is empty." << std::endl;
    exit(EXIT_FAILURE);
  }

  boost::property_tree::ptree xml_tree;

  std::ifstream is;
  is.open(model_filename, std::ifstream::in);

  boost::property_tree::read_xml(is, xml_tree);

  is.close();

  boost::property_tree::ptree info_ptree;
  boost::property_tree::ptree ensemble_ptree;

  BOOST_FOREACH(const boost::property_tree::ptree::value_type& node, xml_tree.get_child("ranker")) {
    if (node.first == "info")
      info_ptree = node.second;
    else if (node.first == "ensemble")
      ensemble_ptree = node.second;
  }

  std::string ranker_type = info_ptree.get<std::string>("type");
  if (ranker_type == forests::Mart::NAME_)
    return std::shared_ptr<LTR_Algorithm>(
        new forests::Mart(info_ptree, ensemble_ptree));
  if (ranker_type == forests::LambdaMart::NAME_)
    return std::shared_ptr<LTR_Algorithm>(
        new forests::LambdaMart(info_ptree, ensemble_ptree));
  if (ranker_type == forests::MatrixNet::NAME_)
    return std::shared_ptr<LTR_Algorithm>(
        new forests::MatrixNet(info_ptree, ensemble_ptree));
  //Coordinate Ascent add by Chiara Pierucci Andrea Battistini      
  if (ranker_type == linear::CoordinateAscent::NAME_)
    return std::shared_ptr<LTR_Algorithm>(
        new linear::CoordinateAscent(info_ptree, ensemble_ptree));      

  return NULL;
}

}  // namespace learning
}  // namespace quickrank
