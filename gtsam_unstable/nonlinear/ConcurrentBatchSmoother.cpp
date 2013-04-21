/* ----------------------------------------------------------------------------

 * GTSAM Copyright 2010, Georgia Tech Research Corporation, 
 * Atlanta, Georgia 30332-0415
 * All Rights Reserved
 * Authors: Frank Dellaert, et al. (see THANKS for the full author list)

 * See LICENSE for the license information

 * -------------------------------------------------------------------------- */

/**
 * @file    ConcurrentBatchSmoother.cpp
 * @brief   A Levenberg-Marquardt Batch Smoother that implements the
 *          Concurrent Filtering and Smoothing interface.
 * @author  Stephen Williams
 */

#include <gtsam_unstable/nonlinear/ConcurrentBatchSmoother.h>
#include <gtsam/nonlinear/LinearContainerFactor.h>
#include <gtsam/linear/GaussianJunctionTree.h>
#include <gtsam/base/timing.h>
#include <gtsam/base/debug.h>

namespace gtsam {

/* ************************************************************************* */
void ConcurrentBatchSmoother::print(const std::string& s, const KeyFormatter& keyFormatter) const {
  std::cout << s;
  std::cout << "  Factors:" << std::endl;
  BOOST_FOREACH(const NonlinearFactor::shared_ptr& factor, factors_) {
    PrintNonlinearFactor(factor, "    ", keyFormatter);
  }
  theta_.print("Values:\n");
}

/* ************************************************************************* */
bool ConcurrentBatchSmoother::equals(const ConcurrentSmoother& rhs, double tol) const {
  const ConcurrentBatchSmoother* smoother = dynamic_cast<const ConcurrentBatchSmoother*>(&rhs);
  return smoother
      && factors_.equals(smoother->factors_)
      && theta_.equals(smoother->theta_)
      && ordering_.equals(smoother->ordering_)
      && delta_.equals(smoother->delta_)
      && separatorValues_.equals(smoother->separatorValues_);
}

/* ************************************************************************* */
ConcurrentBatchSmoother::Result ConcurrentBatchSmoother::update(const NonlinearFactorGraph& newFactors, const Values& newTheta) {

  gttic(update);

  // Create the return result meta-data
  Result result;

  // Update all of the internal variables with the new information
  gttic(augment_system);
  {
    // Add the new variables to theta
    theta_.insert(newTheta);
    // Add new variables to the end of the ordering
    std::vector<size_t> dims;
    dims.reserve(newTheta.size());
    BOOST_FOREACH(const Values::ConstKeyValuePair& key_value, newTheta) {
      ordering_.push_back(key_value.key);
      dims.push_back(key_value.value.dim());
    }
    // Augment Delta
    delta_.append(dims);
    for(size_t i = delta_.size() - dims.size(); i < delta_.size(); ++i) {
      delta_[i].setZero();
    }
    // Add the new factors to the graph, updating the variable index
    insertFactors(newFactors);
  }
  gttoc(augment_system);

  if(factors_.size() > 0) {
    // Reorder the system to ensure efficient optimization (and marginalization) performance
    gttic(reorder);
    reorder();
    gttoc(reorder);

    // Optimize the factors using a modified version of L-M
    gttic(optimize);
    result = optimize();
    gttoc(optimize);
  }

  gttoc(update);

  return result;
}

/* ************************************************************************* */
void ConcurrentBatchSmoother::presync() {

  gttic(presync);

  // TODO: Don't let the length of this code fool you. There is considerable work being done here.
  // When we start multi-threading the filter and smoother, it would be ideal if the pre-sync stage
  // could happen before the filter is stopped.

  // Calculate the marginal on the separator from the smoother factors
  if(separatorValues_.size() > 0) {
    updateSmootherSummarization();
  }

  gttoc(presync);
}

/* ************************************************************************* */
void ConcurrentBatchSmoother::getSummarizedFactors(NonlinearFactorGraph& summarizedFactors, Values& separatorValues) {

  gttic(get_summarized_factors);

  // Copy the previous calculated smoother summarization factors into the output
  summarizedFactors.push_back(smootherSummarization_);

  // Copy the separator values into the output
  separatorValues.insert(separatorValues_);

  gttoc(get_summarized_factors);
}

/* ************************************************************************* */
void ConcurrentBatchSmoother::synchronize(const NonlinearFactorGraph& smootherFactors, const Values& smootherValues,
    const NonlinearFactorGraph& summarizedFactors, const Values& separatorValues) {

  gttic(synchronize);

  // Remove the previous filter summarization from the graph
  removeFactors(filterSummarizationSlots_);

  // Insert new linpoints into the values, augment the ordering, and store new dims to augment delta
  std::vector<size_t> dims;
  dims.reserve(smootherValues.size() + separatorValues.size());
  BOOST_FOREACH(const Values::ConstKeyValuePair& key_value, smootherValues) {
    Values::iterator iter = theta_.find(key_value.key);
    if(iter == theta_.end()) {
      theta_.insert(key_value.key, key_value.value);
      ordering_.push_back(key_value.key);
      dims.push_back(key_value.value.dim());
    } else {
      iter->value = key_value.value;
    }
  }
  BOOST_FOREACH(const Values::ConstKeyValuePair& key_value, separatorValues) {
    Values::iterator iter = theta_.find(key_value.key);
    if(iter == theta_.end()) {
      theta_.insert(key_value.key, key_value.value);
      ordering_.push_back(key_value.key);
      dims.push_back(key_value.value.dim());
    } else {
      iter->value = key_value.value;
    }
  }

  // Augment Delta
  delta_.append(dims);
  for(size_t i = delta_.size() - dims.size(); i < delta_.size(); ++i) {
    delta_[i].setZero();
  }

  // Insert the new smoother factors
  insertFactors(smootherFactors);

  // Insert the new filter summarized factors
  filterSummarizationSlots_ = insertFactors(summarizedFactors);

  // Update the list of root keys
  separatorValues_ = separatorValues;

  gttoc(synchronize);
}

/* ************************************************************************* */
void ConcurrentBatchSmoother::postsync() {

  gttic(postsync);

  gttoc(postsync);
}

/* ************************************************************************* */
std::vector<size_t> ConcurrentBatchSmoother::insertFactors(const NonlinearFactorGraph& factors) {

  gttic(insert_factors);

  // create the output vector
  std::vector<size_t> slots;
  slots.reserve(factors.size());

  // Insert the factor into an existing hole in the factor graph, if possible
  BOOST_FOREACH(const NonlinearFactor::shared_ptr& factor, factors) {
    size_t slot;
    if(availableSlots_.size() > 0) {
      slot = availableSlots_.front();
      availableSlots_.pop();
      factors_.replace(slot, factor);
    } else {
      slot = factors_.size();
      factors_.push_back(factor);
    }
    slots.push_back(slot);
  }

  gttoc(insert_factors);

  return slots;
}

/* ************************************************************************* */
void ConcurrentBatchSmoother::removeFactors(const std::vector<size_t>& slots) {

  gttic(remove_factors);

  // For each factor slot to delete...
  SymbolicFactorGraph factors;
  BOOST_FOREACH(size_t slot, slots) {
    // Create a symbolic version for the variable index
    factors.push_back(factors_.at(slot)->symbolic(ordering_));

    // Remove the factor from the graph
    factors_.remove(slot);

    // Mark the factor slot as available
    availableSlots_.push(slot);
  }

  gttoc(remove_factors);
}

/* ************************************************************************* */
void ConcurrentBatchSmoother::reorder() {

  // Recalculate the variable index
  variableIndex_ = VariableIndex(*factors_.symbolic(ordering_));

  // Initialize all variables to group0
  std::vector<int> cmember(variableIndex_.size(), 0);

  // Set all of the separator keys to Group1
  if(separatorValues_.size() > 0) {
    BOOST_FOREACH(const Values::ConstKeyValuePair& key_value, separatorValues_) {
      cmember[ordering_.at(key_value.key)] = 1;
    }
  }

  // Generate the permutation
  Permutation forwardPermutation = *inference::PermutationCOLAMD_(variableIndex_, cmember);

  // Permute the ordering, variable index, and deltas
  ordering_.permuteInPlace(forwardPermutation);
  variableIndex_.permuteInPlace(forwardPermutation);
  delta_.permuteInPlace(forwardPermutation);
}

/* ************************************************************************* */
ConcurrentBatchSmoother::Result ConcurrentBatchSmoother::optimize() {

  // Create output result structure
  Result result;
  result.nonlinearVariables = theta_.size() - separatorValues_.size();
  result.linearVariables = separatorValues_.size();

  // Set optimization parameters
  double lambda = parameters_.lambdaInitial;
  double lambdaFactor = parameters_.lambdaFactor;
  double lambdaUpperBound = parameters_.lambdaUpperBound;
  double lambdaLowerBound = 0.5 / parameters_.lambdaUpperBound;
  size_t maxIterations = parameters_.maxIterations;
  double relativeErrorTol = parameters_.relativeErrorTol;
  double absoluteErrorTol = parameters_.absoluteErrorTol;
  double errorTol = parameters_.errorTol;

  // Create a Values that holds the current evaluation point
  Values evalpoint = theta_.retract(delta_, ordering_);
  result.error = factors_.error(evalpoint);

  // Use a custom optimization loop so the linearization points can be controlled
  double previousError;
  VectorValues newDelta;
  do {
    previousError = result.error;

    // Do next iteration
    gttic(optimizer_iteration);
    {
      // Linearize graph around the linearization point
      GaussianFactorGraph linearFactorGraph = *factors_.linearize(theta_, ordering_);

      // Keep increasing lambda until we make make progress
      while(true) {
        // Add prior factors at the current solution
        gttic(damp);
        GaussianFactorGraph dampedFactorGraph(linearFactorGraph);
        dampedFactorGraph.reserve(linearFactorGraph.size() + delta_.size());
        {
          // for each of the variables, add a prior at the current solution
          for(size_t j=0; j<delta_.size(); ++j) {
            Matrix A = lambda * eye(delta_[j].size());
            Vector b = lambda * delta_[j];
            SharedDiagonal model = noiseModel::Unit::Create(delta_[j].size());
            GaussianFactor::shared_ptr prior(new JacobianFactor(j, A, b, model));
            dampedFactorGraph.push_back(prior);
          }
        }
        gttoc(damp);
        result.lambdas++;

        gttic(solve);
        // Solve Damped Gaussian Factor Graph
        newDelta = GaussianJunctionTree(dampedFactorGraph).optimize(parameters_.getEliminationFunction());
        // update the evalpoint with the new delta
        evalpoint = theta_.retract(newDelta, ordering_);
        gttoc(solve);

        // Evaluate the new error
        gttic(compute_error);
        double error = factors_.error(evalpoint);
        gttoc(compute_error);

        if(error < result.error) {
          // Keep this change
          // Update the error value
          result.error = error;
          // Update the linearization point
          theta_ = evalpoint;
          // Reset the deltas to zeros
          delta_.setZero();
          // Put the linearization points and deltas back for specific variables
          if(separatorValues_.size() > 0) {
            theta_.update(separatorValues_);
            BOOST_FOREACH(const Values::ConstKeyValuePair& key_value, separatorValues_) {
              Index index = ordering_.at(key_value.key);
              delta_.at(index) = newDelta.at(index);
            }
          }
          // Decrease lambda for next time
          lambda /= lambdaFactor;
          if(lambda < lambdaLowerBound) {
            lambda = lambdaLowerBound;
          }
          // End this lambda search iteration
          break;
        } else {
          // Reject this change
          // Increase lambda and continue searching
          lambda *= lambdaFactor;
          if(lambda > lambdaUpperBound) {
            // The maximum lambda has been used. Print a warning and end the search.
            std::cout << "Warning:  Levenberg-Marquardt giving up because cannot decrease error with maximum lambda" << std::endl;
            break;
          }
        }
      } // end while
    }
    gttoc(optimizer_iteration);

    result.iterations++;
  } while(result.iterations < maxIterations &&
      !checkConvergence(relativeErrorTol, absoluteErrorTol, errorTol, previousError, result.error, NonlinearOptimizerParams::SILENT));

  return result;
}

/* ************************************************************************* */
void ConcurrentBatchSmoother::updateSmootherSummarization() {

  // The smoother summarization factors are the resulting marginal factors on the separator
  // variables that result from marginalizing out all of the other variables
  // These marginal factors will be cached for later transmission to the filter using
  // linear container factors

  // Clear out any existing smoother summarized factors
  smootherSummarization_.resize(0);

  // Reorder the system so that the separator keys are eliminated last
  // TODO: This is currently being done twice: here and in 'update'. Fix it.
  reorder();

  // Create the linear factor graph
  GaussianFactorGraph linearFactorGraph = *factors_.linearize(theta_, ordering_);

  // Construct an elimination tree to perform sparse elimination
  std::vector<EliminationForest::shared_ptr> forest( EliminationForest::Create(linearFactorGraph, variableIndex_) );

  // This is a forest. Only the top-most node/index of each tree needs to be eliminated; all of the children will be eliminated automatically
  // Find the subset of nodes/keys that must be eliminated
  std::set<Index> indicesToEliminate;
  BOOST_FOREACH(const Values::ConstKeyValuePair& key_value, theta_) {
    indicesToEliminate.insert(ordering_.at(key_value.key));
  }
  BOOST_FOREACH(const Values::ConstKeyValuePair& key_value, separatorValues_) {
    indicesToEliminate.erase(ordering_.at(key_value.key));
  }
  std::vector<Index> indices(indicesToEliminate.begin(), indicesToEliminate.end());
  BOOST_FOREACH(Index index, indices) {
    EliminationForest::removeChildrenIndices(indicesToEliminate, forest.at(index));
  }

  // Eliminate each top-most key, returning a Gaussian Factor on some of the remaining variables
  // Convert the marginal factors into Linear Container Factors and store
  BOOST_FOREACH(Index index, indicesToEliminate) {
    GaussianFactor::shared_ptr gaussianFactor = forest.at(index)->eliminateRecursive(parameters_.getEliminationFunction());
    if(gaussianFactor->size() > 0) {
      LinearContainerFactor::shared_ptr marginalFactor(new LinearContainerFactor(gaussianFactor, ordering_, theta_));
      smootherSummarization_.push_back(marginalFactor);
    }
  }

}

/* ************************************************************************* */
void ConcurrentBatchSmoother::PrintNonlinearFactor(const NonlinearFactor::shared_ptr& factor, const std::string& indent, const KeyFormatter& keyFormatter) {
  std::cout << indent;
  if(factor) {
    if(boost::dynamic_pointer_cast<LinearContainerFactor>(factor)) {
      std::cout << "l( ";
    } else {
      std::cout << "f( ";
    }
    BOOST_FOREACH(Key key, *factor) {
      std::cout << keyFormatter(key) << " ";
    }
    std::cout << ")" << std::endl;
  } else {
    std::cout << "{ NULL }" << std::endl;
  }
}

/* ************************************************************************* */
void ConcurrentBatchSmoother::PrintLinearFactor(const GaussianFactor::shared_ptr& factor, const Ordering& ordering, const std::string& indent, const KeyFormatter& keyFormatter) {
  std::cout << indent;
  if(factor) {
    std::cout << "g( ";
    BOOST_FOREACH(Index index, *factor) {
      std::cout << keyFormatter(ordering.key(index)) << " ";
    }
    std::cout << ")" << std::endl;
  } else {
    std::cout << "{ NULL }" << std::endl;
  }
}

/* ************************************************************************* */
std::vector<Index> ConcurrentBatchSmoother::EliminationForest::ComputeParents(const VariableIndex& structure) {
  // Number of factors and variables
  const size_t m = structure.nFactors();
  const size_t n = structure.size();

  static const Index none = std::numeric_limits<Index>::max();

  // Allocate result parent vector and vector of last factor columns
  std::vector<Index> parents(n, none);
  std::vector<Index> prevCol(m, none);

  // for column j \in 1 to n do
  for (Index j = 0; j < n; j++) {
    // for row i \in Struct[A*j] do
    BOOST_FOREACH(const size_t i, structure[j]) {
      if (prevCol[i] != none) {
        Index k = prevCol[i];
        // find root r of the current tree that contains k
        Index r = k;
        while (parents[r] != none)
          r = parents[r];
        if (r != j) parents[r] = j;
      }
      prevCol[i] = j;
    }
  }

  return parents;
}

/* ************************************************************************* */
std::vector<ConcurrentBatchSmoother::EliminationForest::shared_ptr> ConcurrentBatchSmoother::EliminationForest::Create(const GaussianFactorGraph& factorGraph, const VariableIndex& structure) {
  // Compute the tree structure
  std::vector<Index> parents(ComputeParents(structure));

  // Number of variables
  const size_t n = structure.size();

  static const Index none = std::numeric_limits<Index>::max();

  // Create tree structure
  std::vector<shared_ptr> trees(n);
  for (Index k = 1; k <= n; k++) {
    Index j = n - k;  // Start at the last variable and loop down to 0
    trees[j].reset(new EliminationForest(j));  // Create a new node on this variable
    if (parents[j] != none)  // If this node has a parent, add it to the parent's children
      trees[parents[j]]->add(trees[j]);
  }

  // Hang factors in right places
  BOOST_FOREACH(const GaussianFactor::shared_ptr& factor, factorGraph) {
    if(factor && factor->size() > 0) {
      Index j = *std::min_element(factor->begin(), factor->end());
      if(j < structure.size())
        trees[j]->add(factor);
    }
  }

  return trees;
}

/* ************************************************************************* */
GaussianFactor::shared_ptr ConcurrentBatchSmoother::EliminationForest::eliminateRecursive(GaussianFactorGraph::Eliminate function) {

  // Create the list of factors to be eliminated, initially empty, and reserve space
  GaussianFactorGraph factors;
  factors.reserve(this->factors_.size() + this->subTrees_.size());

  // Add all factors associated with the current node
  factors.push_back(this->factors_.begin(), this->factors_.end());

  // for all subtrees, eliminate into Bayes net and a separator factor, added to [factors]
  BOOST_FOREACH(const shared_ptr& child, subTrees_)
    factors.push_back(child->eliminateRecursive(function));

  // Combine all factors (from this node and from subtrees) into a joint factor
  GaussianFactorGraph::EliminationResult eliminated(function(factors, 1));

  return eliminated.second;
}

/* ************************************************************************* */
void ConcurrentBatchSmoother::EliminationForest::removeChildrenIndices(std::set<Index>& indices, const ConcurrentBatchSmoother::EliminationForest::shared_ptr& tree) {
  BOOST_FOREACH(const EliminationForest::shared_ptr& child, tree->children()) {
    indices.erase(child->key());
    removeChildrenIndices(indices, child);
  }
}

/* ************************************************************************* */

}/// namespace gtsam