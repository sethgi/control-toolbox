/**********************************************************************************************************************
This file is part of the Control Toolbox (https://github.com/ethz-adrl/control-toolbox), copyright by ETH Zurich.
Licensed under the BSD-2 license (see LICENSE file in main directory)
 **********************************************************************************************************************/

#pragma once

#include <chrono>

#include <gtest/gtest.h>

#include "../testSystems/LinearOscillator.h"

namespace ct {
namespace optcon {
namespace example {

using namespace ct::core;
using namespace ct::optcon;

using std::shared_ptr;


/*!
 * This unit test considers a variety of different solver/algorithm options for NLOC, and
 * explicitly compares HPIPM and GNRiccati solvers against each other.
 */
TEST(LinearSystemsSolverComparison, LinearSystemsSolverComparison)
{
    typedef NLOptConSolver<state_dim, control_dim, state_dim / 2, state_dim / 2> NLOptConSolver;

    // count executed tests
    size_t testCounter = 0;

    // desired final state
    Eigen::Vector2d x_final;
    x_final << 20, 0;

    // given initial state
    StateVector<state_dim> initState;
    initState.setZero();
    initState(1) = 1.0;

    // provide algorithm settings
    NLOptConSettings nloc_settings;
    nloc_settings.epsilon = 0.0;
    nloc_settings.recordSmallestEigenvalue = false;
    nloc_settings.fixedHessianCorrection = false;
    nloc_settings.dt = 0.01;
    nloc_settings.discretization = NLOptConSettings::APPROXIMATION::FORWARD_EULER;  // default approximation
    nloc_settings.printSummary = false;

    // loop through all solver classes
    for (int algClass = 0; algClass < NLOptConSettings::NLOCP_ALGORITHM::NUM_TYPES; algClass++)
    {
        nloc_settings.nlocp_algorithm = static_cast<NLOptConSettings::NLOCP_ALGORITHM>(algClass);

        // switch line search on or off
        for (int toggleLS = 0; toggleLS <= 1; toggleLS++)
        {
            nloc_settings.lineSearchSettings.type = static_cast<LineSearchSettings::TYPE>(toggleLS);

            // toggle between single and multi-threading
            for (size_t nThreads = 1; nThreads < 5; nThreads = nThreads + 3)
            {
                nloc_settings.nThreads = nThreads;

                // toggle between iLQR/GNMS and hybrid methods with K_shot !=1
                for (size_t kshot = 1; kshot < 11; kshot = kshot + 9)
                {
                    nloc_settings.K_shot = kshot;

                    if (kshot > 1 && nloc_settings.nlocp_algorithm == NLOptConSettings::NLOCP_ALGORITHM::ILQR)
                        continue;  // proceed to next test case

                    // toggle sensitivity integrator
                    for (size_t sensInt = 0; sensInt <= 1; sensInt++)
                    {
                        nloc_settings.useSensitivityIntegrator = bool(sensInt);

                        // toggle over simulation time-steps
                        for (size_t ksim = 1; ksim <= 5; ksim = ksim + 4)
                        {
                            nloc_settings.K_sim = ksim;

                            // catch special case, simulation sub-time steps only make sense when sensitivity integrator active
                            if ((nloc_settings.useSensitivityIntegrator == false) && (ksim > 1))
                                continue;  // proceed to next test case

                            // toggle integrator type
                            for (size_t integratortype = 0; integratortype <= 1; integratortype++)
                            {
                                if (integratortype == 0)
                                    nloc_settings.integrator = ct::core::IntegrationType::EULERCT;
                                else if (integratortype == 1 && nloc_settings.useSensitivityIntegrator == true)
                                {
                                    // use RK4 with exactly integrated sensitivities
                                    nloc_settings.integrator = ct::core::IntegrationType::RK4CT;
                                }
                                else
                                    continue;  // proceed to next test case

                                shared_ptr<ControlledSystem<state_dim, control_dim>> nonlinearSystem(
                                    new LinearOscillator());
                                shared_ptr<LinearSystem<state_dim, control_dim>> analyticLinearSystem(
                                    new LinearOscillatorLinear());
                                shared_ptr<CostFunctionQuadratic<state_dim, control_dim>> costFunction =
                                    tpl::createCostFunctionLinearOscillator<double>(x_final);

                                // times
                                ct::core::Time tf = 1.0;
                                size_t nSteps = nloc_settings.computeK(tf);

                                // initial controller
                                StateVectorArray<state_dim> x0(nSteps + 1, initState);
                                ControlVector<control_dim> uff;
                                uff << kStiffness * initState(0);
                                ControlVectorArray<control_dim> u0(nSteps, uff);

                                FeedbackArray<state_dim, control_dim> u0_fb(
                                    nSteps, FeedbackMatrix<state_dim, control_dim>::Zero());
                                ControlVectorArray<control_dim> u0_ff(nSteps, ControlVector<control_dim>::Zero());

                                NLOptConSolver::Policy_t initController(x0, u0, u0_fb, nloc_settings.dt);

                                // construct OptCon Problem
                                ContinuousOptConProblem<state_dim, control_dim> optConProblem(
                                    tf, x0[0], nonlinearSystem, costFunction, analyticLinearSystem);

                                // NLOC solver employing GNRiccati
                                nloc_settings.lqocp_solver = NLOptConSettings::LQOCP_SOLVER::GNRICCATI_SOLVER;
                                NLOptConSolver solver_gnriccati(optConProblem, nloc_settings);
                                solver_gnriccati.configure(nloc_settings);
                                solver_gnriccati.setInitialGuess(initController);
                                //! run two iterations to solve LQ problem
                                solver_gnriccati.runIteration();
                                solver_gnriccati.runIteration();


                                // NLOC solver employing HPIPM
                                nloc_settings.lqocp_solver = NLOptConSettings::LQOCP_SOLVER::HPIPM_SOLVER;
                                NLOptConSolver solver_hpipm(optConProblem, nloc_settings);
                                solver_hpipm.configure(nloc_settings);
                                solver_hpipm.setInitialGuess(initController);
                                //! run two iterations to solve LQ problem
                                solver_hpipm.runIteration();
                                solver_hpipm.runIteration();

                                //! retrieve summaries
                                const SummaryAllIterations<double>& sumGn = solver_gnriccati.getBackend()->getSummary();
                                const SummaryAllIterations<double>& sumHpipm = solver_hpipm.getBackend()->getSummary();

                                //! check that all logs from both solvers are identical
                                // first iteration
                                ASSERT_NEAR(sumGn.lx_norms.back(), sumHpipm.lx_norms.back(), 1e-6);
                                ASSERT_NEAR(sumGn.lu_norms.back(), sumHpipm.lu_norms.back(), 1e-6);
                                ASSERT_NEAR(sumGn.defect_l1_norms.back(), sumHpipm.defect_l1_norms.back(), 1e-6);
                                ASSERT_NEAR(sumGn.defect_l2_norms.back(), sumHpipm.defect_l2_norms.back(), 1e-6);
                                ASSERT_NEAR(sumGn.merits.back(), sumHpipm.merits.back(), 1e-6);

                                // second iteration
                                ASSERT_NEAR(sumGn.lx_norms.front(), sumHpipm.lx_norms.front(), 1e-6);
                                ASSERT_NEAR(sumGn.lu_norms.front(), sumHpipm.lu_norms.front(), 1e-6);
                                ASSERT_NEAR(sumGn.defect_l1_norms.front(), sumHpipm.defect_l1_norms.front(), 1e-6);
                                ASSERT_NEAR(sumGn.defect_l2_norms.front(), sumHpipm.defect_l2_norms.front(), 1e-6);
                                ASSERT_NEAR(sumGn.merits.front(), sumHpipm.merits.front(), 1e-6);

                                testCounter++;

                            }  // toggle integrator type
                        }      // toggle simulation time steps
                    }          // toggle sensitivity integrator
                }              // toggle k_shot
            }                  // toggle multi-threading / single-threading
        }                      // toggle line-search
    }                          // toggle solver class

    std::cout << "Performed " << testCounter << " successful NLOC tests with linear systems" << std::endl;

}  // end TEST


}  // namespace example
}  // namespace optcon
}  // namespace ct
