/*
 * File: panglossStruct.cpp
 *
 * Implements metric MDS (multi-dimensional scaling)
 * for pangloss kmers
 */

#include "pangloss.hpp"

arma::mat metricMDS(const arma::mat& populationMatrix, const int dimensions, const unsigned int threads)
{
   /*
    * Metric MDS
    *
    * 1) P^2 -> matrix with elements which are distances squared
    * 2) J = I - n^-1(II') - II' is a square matrix of ones
    * 3) B = -0.5JP^2J
    * 4) Decompose B into eigenvalues
    * 5) MDS components = eigenvectors * eigenvalues
    */
   const unsigned int matSize = populationMatrix.n_rows;

   // Step 1)
   arma::mat P = arma::square(dissimiliarityMatrix(populationMatrix, threads));

   // Step 2)
   arma::mat J = arma::eye<arma::mat>(matSize, matSize)
      - 1/double(matSize)*arma::ones<arma::mat>(matSize, matSize);

   // Step 3)
   arma::mat B = -0.5 * J * P * J;

   // Step 4)
   arma::vec eigval;
   arma::mat eigvec;

   arma::eig_sym(eigval, eigvec, B);

   // Step 5)
   // Eigenvalues returned are sorted ascending, so want to reverse order
   arma::mat mds = fliplr(eigvec * diagmat(sqrt(eigval)));

   return mds.cols(0, dimensions - 1);
}

// Distance between all rows. 0/1 elements only
arma::mat dissimiliarityMatrix(const arma::mat& inMat, const unsigned int threads)
{
   const unsigned int matSize = inMat.n_rows;
   arma::mat dist(matSize, matSize);

#ifndef NO_THREAD
   std::queue<std::future<distance_element>> distance_calculations;
#endif

   // Loop through upper triangle
   for (unsigned int i = 0; i < matSize; ++i)
   {
      arma::vec ref_row = inMat.row(i);
      for (unsigned int j = i; j < matSize; j++)
      {
         if (i == j)
         {
            dist(i, j) = 0;
         }
         else
         {
#ifdef NO_THREAD
            dist(i, j) = distanceFunction(ref_row, inMat.row(j));
            dist(j, i) = dist(i, j); // Set symmetric elements
#else
            if (distance_calculations.size() == threads)
            {
               distance_element d = distance_calculations.front().get();
               distance_calculations.pop();

               dist(d.row, d.col) = d.distance;
               dist(d.col, d.row) = d.distance;
            }
            arma::vec new_row = inMat.row(j);
            distance_calculations.push(std::async(threadDistance, i, j, std::cref(ref_row), std::cref(new_row)));
#endif
         }
      }
   }

   while (distance_calculations.size() > 0)
   {
      distance_element d = distance_calculations.front().get();
      distance_calculations.pop();

      dist(d.row, d.col) = d.distance;
      dist(d.col, d.row) = d.distance;
   }

   return dist;
}

distance_element threadDistance(const unsigned int i, const unsigned int j, const arma::vec& row_1, const arma::vec& row_2)
{
   distance_element dist_el;

   dist_el.row = i;
   dist_el.col = j;
   dist_el.distance = distanceFunction(row_1, row_2);

   return dist_el;
}

double distanceFunction(const arma::vec& vec_1, const arma::vec& vec_2)
{
   return accu(abs(vec_1 - vec_2));
}
