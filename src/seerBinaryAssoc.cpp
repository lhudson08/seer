/*
 * File: seerBinaryAssoc.cpp
 *
 * Implements logistic regression association tests for seer
 *
 */

#include "seer.hpp"

// Logistic fit without covariates
void logisticTest(Kmer& k, const arma::vec& y_train)
{
   // Train classifier
   arma::mat x_train = k.get_x();
   doLogit(k, y_train, x_train);
}

// Logistic fit with covariates
void logisticTest(Kmer& k, const arma::vec& y_train, const arma::mat& mds)
{
   // Train classifier
   arma::mat x_train = arma::join_rows(k.get_x(), mds);
   doLogit(k, y_train, x_train);
}

void doLogit(Kmer& k, const arma::vec& y_train, const arma::mat& x_train)
{
   arma::mat x_design = join_rows(arma::mat(x_train.n_rows,1,arma::fill::ones), x_train);
   column_vector starting_point(x_design.n_cols);

   starting_point(0) = log(mean(y_train)/(1 - mean(y_train)));
   for (size_t i = 1; i < x_design.n_cols; ++i)
   {
      // Seems to need to be >0 to get bfgs to converge
      starting_point(i) = 1;
   }

   try
   {
      // Use BFGS optimiser in dlib to maximise likelihood function by chaging the
      // b vector, which will end in starting_point
      dlib::find_max(dlib::bfgs_search_strategy(),
                  dlib::objective_delta_stop_strategy(convergence_limit),
                  LogitLikelihood(x_train, y_train), LogitLikelihoodGradient(x_train, y_train),
                  starting_point, -1);

      // Extract beta
      arma::vec b_vector = dlib_to_arma(starting_point);
      double b_1 = b_vector(1);
      k.beta(b_1);

      // Extract p-value
      //
      //
      // W = B_1 / SE(B_1) ~ N(0,1)
      //
      // In the special case of a logistic regression, abs can be taken rather
      // than ^2 as responses are 0 or 1
      //
      double se = pow(varCovarMat(x_design, b_vector)(1,1), 0.5);
      k.standard_error(se);

      double W = std::abs(b_1) / se; // null hypothesis b_1 = 0
      k.p_val(normalPval(W));

#ifdef SEER_DEBUG
      std::cerr << "Wald statistic: " << W << "\n";
      std::cerr << "p-value: " << k.p_val() << "\n";
#endif
   }
   // Sometimes won't converge, use N-R instead
   catch (std::exception& e)
   {
      k.add_comment("bfgs-fail");
      newtonRaphson(k, y_train, x_design);
   }
}

void newtonRaphson(Kmer& k, const arma::vec& y_train, const arma::mat& x_design, const bool firth)
{
   // Keep iterations to track convergence
   // Also useful to keep second derivative, for calculating p-value
   std::vector<arma::vec> parameter_iterations;
   arma::mat var_covar_mat;

   // Could get starting point from a linear regression, which is fast
   // and will reduce number of n-r iterations
   // Set up design matrix, and calculate (X'X)^-1
   // Seems more reliable to go for b = 0, plus a non-zero intercept
   // See: doi:10.1016/S0169-2607(02)00088-3
   arma::vec starting_point = arma::zeros(x_design.n_cols);
   starting_point(0) = log(mean(y_train)/(1 - mean(y_train)));

   parameter_iterations.push_back(starting_point);

   for (unsigned int i = 0; i < max_nr_iterations; ++i)
   {
      arma::vec b0 = parameter_iterations.back();
      arma::vec y_pred = predictLogitProbs(x_design, b0);

      arma::vec b1 = b0;
      var_covar_mat = inv_covar(x_design.t() * diagmat(y_pred % (arma::ones(y_pred.n_rows) - y_pred)) * x_design);
      if (firth)
      {
         // Firth logistic regression
         // See: DOI: 10.1002/sim.1047
         arma::mat W = diagmat(y_pred * (1 - y_pred));
         // Hat matrix
         // Note: W is diagonal so X.t() * W * X is still sympd
         arma::mat H = sqrt(W) * x_design * inv_covar(x_design.t() * W * x_design) * x_design.t() * sqrt(W);

         arma::vec correction(y_train.n_rows);
         correction.fill(0.5);

         b1 += var_covar_mat * (y_train - y_pred + diagmat(H) % (correction - y_pred));
      }
      else
      {
         b1 += var_covar_mat * x_design.t() * (y_train - y_pred);
      }
      parameter_iterations.push_back(b1);

      if (std::abs(b1(1) - b0(1)) < convergence_limit)
      {
         break;
      }
   }

#ifdef SEER_DEBUG
   std::cerr << "Number of iterations: " << parameter_iterations.size() << "\n";
#endif
   // If convergence not reached, try Firth logistic regression
   if (parameter_iterations.size() == max_nr_iterations)
   {
      if (!firth)
      {
         k.add_comment("nr-fail");
         newtonRaphson(k, y_train, x_design, 1);
      }
      else
      {
         k.add_comment("firth-fail");
      }
   }
   else
   {
      k.beta(parameter_iterations.back()(1));

      double se = pow(var_covar_mat(1,1), 0.5);
      k.standard_error(se);

      double W = std::abs(k.beta()) / se;
      k.p_val(normalPval(W));

#ifdef SEER_DEBUG
      std::cerr << "Wald statistic: " << W << "\n";
      std::cerr << "p-value: " << k.p_val() << "\n";
#endif
   }
}

// Returns var-covar matrix for logistic function
arma::mat varCovarMat(const arma::mat& x, const arma::mat& b)
{
   // var-covar matrix = inv(I)
   // where I is the Fisher information matrix
   // I = d^2/d(b^2)[log L]
   //
   // see http://czep.net/stat/mlelr.pdf

   // First get logit of x values using parameters from fit, and transform to
   // p(1-p)
   arma::vec y_pred = predictLogitProbs(x, b);
   arma::vec y_trans = y_pred % (1 - y_pred);

   // Fill elements of I, which are sums of element by element vector multiples
   arma::mat I(b.n_elem, b.n_elem);
   unsigned int j_max = I.n_rows;
   for (unsigned int i = 0; i<I.n_cols; ++i)
   {
      for (unsigned int j = i; j < j_max; j++)
      {
         I(i,j) = accu(y_trans % x.col(i) % x.col(j));
         if (i != j)
         {
            I(j,i) = I(i,j); // I is symmetric - only need to calculate upper triangle
         }
      }
   }

   return inv_covar(I);
}

// returns y = logit(bx)
arma::vec predictLogitProbs(const arma::mat& x, const arma::vec& b)
{
   const arma::vec exponents = x * b;
   const arma::vec y = 1.0 / (1.0 + arma::exp(-exponents));

   return y;
}