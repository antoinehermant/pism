// Copyright (C) 2012, 2013, 2014, 2015  David Maxwell and Constantine Khroulev
//
// This file is part of PISM.
//
// PISM is free software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation; either version 3 of the License, or (at your option) any later
// version.
//
// PISM is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License
// along with PISM; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

#include "IP_SSATaucTikhonovGNSolver.hh"
#include <cassert>
#include "TerminationReason.hh"
#include "pism_options.hh"

namespace pism {

IP_SSATaucTikhonovGNSolver::IP_SSATaucTikhonovGNSolver(IP_SSATaucForwardProblem &ssaforward,
                                                       DesignVec &d0, StateVec &u_obs, double eta,
                                                       IPInnerProductFunctional<DesignVec> &designFunctional,
                                                       IPInnerProductFunctional<StateVec> &stateFunctional)
  : m_ssaforward(ssaforward), m_d0(d0), m_u_obs(u_obs), m_eta(eta),
    m_designFunctional(designFunctional), m_stateFunctional(stateFunctional),
    m_target_misfit(0.0)
{
  PetscErrorCode ierr;
  ierr = this->construct();
  assert(ierr==0);
}

IP_SSATaucTikhonovGNSolver::~IP_SSATaucTikhonovGNSolver() {
  // empty
}


PetscErrorCode IP_SSATaucTikhonovGNSolver::construct() {
  PetscErrorCode ierr;
  const IceGrid &grid = *m_d0.get_grid();
  m_comm = grid.com;

  unsigned int design_stencil_width = m_d0.get_stencil_width();
  unsigned int state_stencil_width = m_u_obs.get_stencil_width();

  m_x.create(grid, "x", WITH_GHOSTS, design_stencil_width);

  m_tmp_D1Global.create(grid, "work vector", WITHOUT_GHOSTS, 0);
  m_tmp_D2Global.create(grid, "work vector", WITHOUT_GHOSTS, 0);
  m_tmp_S1Global.create(grid, "work vector", WITHOUT_GHOSTS, 0);
  m_tmp_S2Global.create(grid, "work vector", WITHOUT_GHOSTS, 0);

  m_tmp_D1Local.create(grid, "work vector", WITH_GHOSTS, design_stencil_width);
  m_tmp_D2Local.create(grid, "work vector", WITH_GHOSTS, design_stencil_width);
  m_tmp_S1Local.create(grid, "work vector", WITH_GHOSTS, state_stencil_width);
  m_tmp_S2Local.create(grid, "work vector", WITH_GHOSTS, state_stencil_width);

  m_GN_rhs.create(grid, "GN_rhs", WITHOUT_GHOSTS, 0);

  m_dGlobal.create(grid, "d (sans ghosts)", WITHOUT_GHOSTS, 0);
  m_d.create(grid, "d", WITH_GHOSTS, design_stencil_width);
  m_d_diff.create(grid, "d_diff", WITH_GHOSTS, design_stencil_width);
  m_d_diff_lin.create(grid, "d_diff linearized", WITH_GHOSTS, design_stencil_width);
  m_h.create(grid, "h", WITH_GHOSTS, design_stencil_width);
  m_hGlobal.create(grid, "h (sans ghosts)", WITHOUT_GHOSTS);
  
  m_dalpha_rhs.create(grid, "dalpha rhs", WITHOUT_GHOSTS);
  m_dh_dalpha.create(grid, "dh_dalpha", WITH_GHOSTS, design_stencil_width);
  m_dh_dalphaGlobal.create(grid, "dh_dalpha", WITHOUT_GHOSTS);
  m_u_diff.create(grid, "du", WITH_GHOSTS, state_stencil_width);

  m_grad_design.create(grid, "grad design", WITHOUT_GHOSTS);
  m_grad_state.create(grid, "grad design", WITHOUT_GHOSTS);
  m_gradient.create(grid, "grad design", WITHOUT_GHOSTS);

  ierr = KSPCreate(grid.com, m_ksp.rawptr());
  PISM_PETSC_CHK(ierr, "KSPCreate");

  ierr = KSPSetOptionsPrefix(m_ksp, "inv_gn_");
  PISM_PETSC_CHK(ierr, "KSPSetOptionsPrefix");

  double ksp_rtol = 1e-5; // Soft tolerance
  ierr = KSPSetTolerances(m_ksp, ksp_rtol, PETSC_DEFAULT, PETSC_DEFAULT, PETSC_DEFAULT);
  PISM_PETSC_CHK(ierr, "KSPSetTolerances");

  ierr = KSPSetType(m_ksp, KSPCG);
  PISM_PETSC_CHK(ierr, "KSPSetType");

  PC pc;
  ierr = KSPGetPC(m_ksp, &pc);
  PISM_PETSC_CHK(ierr, "KSPGetPC");

  PCSetType(pc, PCNONE);

  ierr = KSPSetFromOptions(m_ksp);
  PISM_PETSC_CHK(ierr, "KSPSetFromOptions");  

  int nLocalNodes  = grid.xm()*grid.ym();
  int nGlobalNodes = grid.Mx()*grid.My();
  MatCreateShell(grid.com, nLocalNodes, nLocalNodes,
                 nGlobalNodes, nGlobalNodes, this, m_mat_GN.rawptr());

  typedef MatrixMultiplyCallback<IP_SSATaucTikhonovGNSolver, &IP_SSATaucTikhonovGNSolver::apply_GN> multCallback;
  ierr = multCallback::connect(m_mat_GN);
  PISM_PETSC_CHK(ierr, "multCallback::connect");

  m_alpha = 1./m_eta;
  m_logalpha = log(m_alpha);

  m_tikhonov_adaptive = options::Bool("-tikhonov_adaptive", "Tikhonov adaptive");
  
  m_iter_max = 1000;
  m_iter_max = options::Integer("-inv_gn_iter_max", "", m_iter_max);

  m_tikhonov_atol = grid.config.get("tikhonov_atol");
  m_tikhonov_rtol = grid.config.get("tikhonov_rtol");
  m_tikhonov_ptol = grid.config.get("tikhonov_ptol");

  return 0;
}

PetscErrorCode IP_SSATaucTikhonovGNSolver::init(TerminationReason::Ptr &reason) {
  m_ssaforward.linearize_at(m_d0,reason);
  return 0;
}

PetscErrorCode IP_SSATaucTikhonovGNSolver::apply_GN(IceModelVec2S &x,IceModelVec2S &y) {
  this->apply_GN(x.get_vec(),y.get_vec());
  return 0; 
}

PetscErrorCode IP_SSATaucTikhonovGNSolver::apply_GN(Vec x, Vec y) {
  PetscErrorCode ierr;

  StateVec &tmp_gS    = m_tmp_S1Global;
  StateVec &Tx        = m_tmp_S1Local;
  DesignVec &tmp_gD   = m_tmp_D1Global;
  DesignVec  &GNx      = m_tmp_D2Global;
  
  // FIXME: Needless copies for now.
  m_x.copy_from_vec(x);

  m_ssaforward.apply_linearization(m_x,Tx);
  Tx.update_ghosts();
  
  m_stateFunctional.interior_product(Tx,tmp_gS);
  
  m_ssaforward.apply_linearization_transpose(tmp_gS,GNx);

  m_designFunctional.interior_product(m_x,tmp_gD);
  GNx.add(m_alpha,tmp_gD);

  ierr = VecCopy(GNx.get_vec(), y);
  PISM_PETSC_CHK(ierr, "VecCopy");

  return 0;
}

PetscErrorCode IP_SSATaucTikhonovGNSolver::assemble_GN_rhs(DesignVec &rhs) {

  rhs.set(0);
  
  m_stateFunctional.interior_product(m_u_diff,m_tmp_S1Global);
  m_ssaforward.apply_linearization_transpose(m_tmp_S1Global,rhs);

  m_designFunctional.interior_product(m_d_diff,m_tmp_D1Global);
  rhs.add(m_alpha,m_tmp_D1Global);
  
  rhs.scale(-1);

  return 0;
}

PetscErrorCode IP_SSATaucTikhonovGNSolver::solve_linearized(TerminationReason::Ptr &reason) {
  PetscErrorCode ierr;

  this->assemble_GN_rhs(m_GN_rhs);

#if PETSC_VERSION_LT(3,5,0)
  ierr = KSPSetOperators(m_ksp,m_mat_GN,m_mat_GN,SAME_NONZERO_PATTERN);
  PISM_PETSC_CHK(ierr, "KSPSetOperators");
#else
  ierr = KSPSetOperators(m_ksp,m_mat_GN,m_mat_GN);
  PISM_PETSC_CHK(ierr, "KSPSetOperators");
#endif
  ierr = KSPSolve(m_ksp,m_GN_rhs.get_vec(),m_hGlobal.get_vec());
  PISM_PETSC_CHK(ierr, "KSPSolve");

  KSPConvergedReason ksp_reason;
  ierr = KSPGetConvergedReason(m_ksp,&ksp_reason);
  PISM_PETSC_CHK(ierr, "KSPGetConvergedReason");
  
  m_h.copy_from(m_hGlobal);

  reason.reset(new KSPTerminationReason(ksp_reason));

  return 0;
}

PetscErrorCode IP_SSATaucTikhonovGNSolver::evaluateGNFunctional(DesignVec &h, double *value) {
  
  m_ssaforward.apply_linearization(h,m_tmp_S1Local);
  m_tmp_S1Local.update_ghosts();
  m_tmp_S1Local.add(1,m_u_diff);
  
  double sValue;
  m_stateFunctional.valueAt(m_tmp_S1Local,&sValue);
  
  
  m_tmp_D1Local.copy_from(m_d_diff);
  m_tmp_D1Local.add(1,h);
  
  double dValue;
  m_designFunctional.valueAt(m_tmp_D1Local,&dValue);
  
  *value = m_alpha*dValue + sValue;

  return 0;
}


PetscErrorCode IP_SSATaucTikhonovGNSolver::check_convergence(TerminationReason::Ptr &reason) {

  double designNorm, stateNorm, sumNorm;
  double dWeight, sWeight;
  dWeight = m_alpha;
  sWeight = 1;

  designNorm = m_grad_design.norm(NORM_2);
  stateNorm  = m_grad_state.norm(NORM_2);

  designNorm *= dWeight;
  stateNorm  *= sWeight;

  sumNorm = m_gradient.norm(NORM_2);

  verbPrintf(2,PETSC_COMM_WORLD,"----------------------------------------------------------\n",
             designNorm,stateNorm,sumNorm);
  verbPrintf(2,PETSC_COMM_WORLD,"IP_SSATaucTikhonovGNSolver Iteration %d: misfit %g; functional %g \n",
             m_iter,sqrt(m_val_state)*m_vel_scale,m_value*m_vel_scale*m_vel_scale);
  if (m_tikhonov_adaptive) {
    verbPrintf(2,PETSC_COMM_WORLD,"alpha %g; log(alpha) %g\n",m_alpha,m_logalpha);
  }
  double relsum = (sumNorm/std::max(designNorm,stateNorm));
  verbPrintf(2,PETSC_COMM_WORLD,"design norm %g stateNorm %g sum %g; relative difference %g\n",
             designNorm,stateNorm,sumNorm,relsum);

  // If we have an adaptive tikhonov parameter, check if we have met
  // this constraint first.
  if (m_tikhonov_adaptive) {
    double disc_ratio = fabs((sqrt(m_val_state)/m_target_misfit) - 1.);
    if (disc_ratio > m_tikhonov_ptol) {
      reason = GenericTerminationReason::keep_iterating();
      return 0;
    }
  }
  
  if (sumNorm < m_tikhonov_atol) {
    reason.reset(new GenericTerminationReason(1,"TIKHONOV_ATOL"));
    return 0;
  }

  if (sumNorm < m_tikhonov_rtol*std::max(designNorm,stateNorm)) {
    reason.reset(new GenericTerminationReason(1,"TIKHONOV_RTOL"));
    return 0;
  }

  if (m_iter>m_iter_max) {
    reason = GenericTerminationReason::max_iter();
  } else {
    reason = GenericTerminationReason::keep_iterating();
  }
  return 0;
}

PetscErrorCode IP_SSATaucTikhonovGNSolver::evaluate_objective_and_gradient(TerminationReason::Ptr &reason) {

  m_ssaforward.linearize_at(m_d,reason);
  if (reason->failed()) {
    return 0;
  }

  m_d_diff.copy_from(m_d);
  m_d_diff.add(-1,m_d0);

  m_u_diff.copy_from(m_ssaforward.solution());
  m_u_diff.add(-1,m_u_obs);

  m_designFunctional.gradientAt(m_d_diff,m_grad_design);

  // The following computes the reduced gradient.
  StateVec &adjointRHS = m_tmp_S1Global;
  m_stateFunctional.gradientAt(m_u_diff,adjointRHS);  
  m_ssaforward.apply_linearization_transpose(adjointRHS,m_grad_state);

  m_gradient.copy_from(m_grad_design);
  m_gradient.scale(m_alpha);    
  m_gradient.add(1,m_grad_state);

  double valDesign, valState;
  m_designFunctional.valueAt(m_d_diff,&valDesign);
  m_stateFunctional.valueAt(m_u_diff,&valState);

  m_val_design = valDesign;
  m_val_state = valState;
  
  m_value = valDesign * m_alpha + valState;

  return 0;
}

PetscErrorCode IP_SSATaucTikhonovGNSolver::linesearch(TerminationReason::Ptr &reason) {
  PetscErrorCode ierr;

  TerminationReason::Ptr step_reason;

  double old_value = m_val_design * m_alpha + m_val_state;

  double descent_derivative;

  m_tmp_D1Global.copy_from(m_h);
  ierr = VecDot(m_gradient.get_vec(),m_tmp_D1Global.get_vec(),&descent_derivative);
  PISM_PETSC_CHK(ierr, "VecDot");
  if (descent_derivative >=0) {
    printf("descent derivative: %g\n",descent_derivative);
    reason.reset(new GenericTerminationReason(-1,"Not descent direction"));
    return 0;
  }

  double alpha = 1;
  m_tmp_D1Local.copy_from(m_d);
  while(true) {
    m_d.add(alpha,m_h);  // Replace with line search.
    this->evaluate_objective_and_gradient(step_reason);
    if (step_reason->succeeded()) {
      if (m_value <= old_value + 1e-3*alpha*descent_derivative) {
        break;
      }
    }
    else {
      printf("forward solve failed in linsearch.  Shrinking.\n");
    }
    alpha *=.5;
    if (alpha<1e-20) {
      printf("alpha= %g; derivative = %g\n",alpha,descent_derivative);
      reason.reset(new GenericTerminationReason(-1,"Too many step shrinks."));
      return 0;
    }
    m_d.copy_from(m_tmp_D1Local);
  }
  
  reason = GenericTerminationReason::success();
  return 0;
}

PetscErrorCode IP_SSATaucTikhonovGNSolver::solve(TerminationReason::Ptr &reason) {


  if (m_target_misfit == 0) {
    throw RuntimeError::formatted("Call set target misfit prior to calling IP_SSATaucTikhonovGNSolver::solve.");
  }

  m_iter = 0;
  m_d.copy_from(m_d0);

  double dlogalpha = 0;

  TerminationReason::Ptr step_reason;

  this->evaluate_objective_and_gradient(step_reason);
  if (step_reason->failed()) {
    reason.reset(new GenericTerminationReason(-1,"Forward solve"));
    reason->set_root_cause(step_reason);
    return 0;
  }

  while(true) {

    this->check_convergence(reason);
    if (reason->done()) {
      return 0;
    }

    if (m_tikhonov_adaptive) {
      m_logalpha += dlogalpha;
      m_alpha = exp(m_logalpha);
    }

    this->solve_linearized(step_reason);
    if (step_reason->failed()) {
      reason.reset(new GenericTerminationReason(-1,"Gauss Newton solve"));
      reason->set_root_cause(step_reason);
      return 0;
    }

    this->linesearch(step_reason);
    if (step_reason->failed()) {
      TerminationReason::Ptr cause = reason;
      reason.reset(new GenericTerminationReason(-1,"Linesearch"));
      reason->set_root_cause(step_reason);
      return 0;
    }

    if (m_tikhonov_adaptive) {
      this->compute_dlogalpha(&dlogalpha,step_reason);
      if (step_reason->failed()) {
        TerminationReason::Ptr cause = reason;
        reason.reset(new GenericTerminationReason(-1,"Tikhonov penalty update"));
        reason->set_root_cause(step_reason);
        return 0;
      }
    }

    m_iter++;
  }
  return 0;
}

PetscErrorCode IP_SSATaucTikhonovGNSolver::compute_dlogalpha(double *dlogalpha, TerminationReason::Ptr &reason) {

  PetscErrorCode ierr;

  // Compute the right-hand side for computing dh/dalpha.
  m_d_diff_lin.copy_from(m_d_diff);
  m_d_diff_lin.add(1,m_h);  
  m_designFunctional.interior_product(m_d_diff_lin,m_dalpha_rhs);
  m_dalpha_rhs.scale(-1);

  // Solve linear equation for dh/dalpha. 
#if PETSC_VERSION_LT(3,5,0)
  ierr = KSPSetOperators(m_ksp,m_mat_GN,m_mat_GN,SAME_NONZERO_PATTERN);
  PISM_PETSC_CHK(ierr, "KSPSetOperators");
#else
  ierr = KSPSetOperators(m_ksp,m_mat_GN,m_mat_GN);
  PISM_PETSC_CHK(ierr, "KSPSetOperators");
#endif
  ierr = KSPSolve(m_ksp,m_dalpha_rhs.get_vec(),m_dh_dalphaGlobal.get_vec());
  PISM_PETSC_CHK(ierr, "KSPSolve");
  m_dh_dalpha.copy_from(m_dh_dalphaGlobal);

  KSPConvergedReason ksp_reason;
  ierr = KSPGetConvergedReason(m_ksp,&ksp_reason);
  PISM_PETSC_CHK(ierr, "KSPGetConvergedReason");
  if (ksp_reason<0) {
    reason.reset(new KSPTerminationReason(ksp_reason));
    return 0;
  }

  // S1Local contains T(h) + F(x) - u_obs, i.e. the linearized misfit field.
  m_ssaforward.apply_linearization(m_h,m_tmp_S1Local);
  m_tmp_S1Local.update_ghosts();
  m_tmp_S1Local.add(1,m_u_diff);

  // Compute linearized discrepancy.
  double disc_sq;
  m_stateFunctional.dot(m_tmp_S1Local,m_tmp_S1Local,&disc_sq);

  // There are a number of equivalent ways to compute the derivative of the 
  // linearized discrepancy with respect to alpha, some of which are cheaper
  // than others to compute.  This equivalency relies, however, on having an 
  // exact solution in the Gauss-Newton step.  Since we only solve this with 
  // a soft tolerance, we lose equivalency.  We attempt a cheap computation,
  // and then do a sanity check (namely that the derivative is positive).
  // If this fails, we compute by a harder way that inherently yields a 
  // positive number.

  double ddisc_sq_dalpha;
  m_designFunctional.dot(m_dh_dalpha,m_d_diff_lin,&ddisc_sq_dalpha);
  ddisc_sq_dalpha *= -2*m_alpha;

  if (ddisc_sq_dalpha <= 0) {
    // Try harder.
    
    verbPrintf(3,PETSC_COMM_WORLD,"Adaptive Tikhonov sanity check failed (dh/dalpha= %g <= 0).  Tighten inv_gn_ksp_rtol?\n",ddisc_sq_dalpha);
    
    // S2Local contains T(dh/dalpha)
    m_ssaforward.apply_linearization(m_dh_dalpha,m_tmp_S2Local);
    m_tmp_S2Local.update_ghosts();

    double ddisc_sq_dalpha_a;
    m_stateFunctional.dot(m_tmp_S2Local,m_tmp_S2Local,&ddisc_sq_dalpha_a);
    double ddisc_sq_dalpha_b;
    m_designFunctional.dot(m_dh_dalpha,m_dh_dalpha,&ddisc_sq_dalpha_b);
    ddisc_sq_dalpha = 2*m_alpha*(ddisc_sq_dalpha_a+m_alpha*ddisc_sq_dalpha_b);

    verbPrintf(3,PETSC_COMM_WORLD,"Adaptive Tikhonov sanity check recovery attempt: dh/dalpha= %g. \n",ddisc_sq_dalpha);

    // This is yet another alternative formula.
    // m_stateFunctional.dot(m_tmp_S1Local,m_tmp_S2Local,&ddisc_sq_dalpha);
    // ddisc_sq_dalpha *= 2;
  }

  // Newton's method formula.
  *dlogalpha = (m_target_misfit*m_target_misfit-disc_sq)/(ddisc_sq_dalpha*m_alpha);

  // It's easy to take steps that are too big when we are far from the solution.
  // So we limit the step size.
  double stepmax = 3;
  if (fabs(*dlogalpha)> stepmax) {
    double sgn = *dlogalpha > 0 ? 1 : -1;
    *dlogalpha = stepmax*sgn;
  }
  
  if (*dlogalpha<0) {
    *dlogalpha*=.5;
  }

  reason = GenericTerminationReason::success();

  return 0;
}

} // end of namespace pism
