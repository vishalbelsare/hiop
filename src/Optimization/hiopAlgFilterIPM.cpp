#include "hiopAlgFilterIPM.hpp"
#include "hiopKKTLinSys.hpp"

#include <cmath>
#include <cstring>
#include <cassert>

namespace hiop
{

hiopAlgFilterIPM::hiopAlgFilterIPM(hiopNlpDenseConstraints* nlp_)
{
  nlp = nlp_;

  it_curr = new hiopIterate(nlp);
  it_trial= it_curr->alloc_clone();
  dir     = it_curr->alloc_clone();

  logbar = new hiopLogBarProblem(nlp);

  _f_nlp = _f_log = 0; 
  _c = nlp->alloc_dual_eq_vec(); 
  _d = nlp->alloc_dual_ineq_vec();

  _grad_f  = nlp->alloc_primal_vec();
  _Jac_c   = nlp->alloc_Jac_c();
  _Jac_d   = nlp->alloc_Jac_d();

  _f_nlp_trial = _f_log_trial = 0;
  _c_trial = nlp->alloc_dual_eq_vec(); 
  _d_trial = nlp->alloc_dual_ineq_vec();

  _grad_f_trial  = nlp->alloc_primal_vec();
  _Jac_c_trial   = nlp->alloc_Jac_c();
  _Jac_d_trial   = nlp->alloc_Jac_d();

  _Hess    = new hiopHessianLowRank(nlp,nlp->options->GetInteger("secant_memory_len"));

  //resid = new hiopResidualFinDimImpl(nlp);
  //resid_trial = new hiopResidualFinDimImpl(nlp);
  resid = new hiopResidual(nlp);
  resid_trial = new hiopResidual(nlp);

  //algorithm parameters parameters
  mu0=_mu  = nlp->options->GetNumeric("mu0"); 
  kappa_mu = nlp->options->GetNumeric("kappa_mu");      //linear decrease factor
  theta_mu = nlp->options->GetNumeric("theta_mu");      //exponent for higher than linear decrease of mu
  tau_min  = nlp->options->GetNumeric("tau_min");       //min value for the fraction-to-the-boundary
  eps_tol  = nlp->options->GetNumeric("tolerance");     //absolute error for the nlp
  kappa_eps= nlp->options->GetNumeric("kappa_eps");     //relative (to mu) error for the log barrier

  kappa1   = nlp->options->GetNumeric("kappa1");        //projection params for the starting point (default 1e-2)
  kappa2   = nlp->options->GetNumeric("kappa2");
  p_smax   = nlp->options->GetNumeric("smax");          //threshold for the magnitude of the multipliers

  max_n_it  = nlp->options->GetInteger("max_iter"); 

  accep_n_it    = nlp->options->GetInteger("acceptable_iterations");
  eps_tol_accep = nlp->options->GetNumeric("acceptable_tolerance");

  dualsUpdateType = nlp->options->GetString("dualsUpdateType")=="lsq"?0:1;     //0 LSQ (default), 1 linear update (more stable)
  dualsInitializ = nlp->options->GetString("dualsInitialization")=="lsq"?0:1;  //0 LSQ (default), 1 set to zero

  gamma_theta = 1e-5; //sufficient progress parameters for the feasibility violation
  gamma_phi=1e-5;     //and log barrier objective
  s_theta=1.1;        //parameters in the switch condition of 
  s_phi=2.3;          // the linearsearch (equation 19) in
  delta=1.;           // the WachterBiegler paper
  eta_phi=1e-4;       // parameter in the Armijo rule
  kappa_Sigma = 1e10; //parameter in resetting the duals to guarantee closedness of the primal-dual logbar Hessian to the primal logbar Hessian
  _tau=fmax(tau_min,1.0-_mu);
  theta_max = 1e7; //temporary - will be updated after ini pt is computed
  theta_min = 1e7; //temporary - will be updated after ini pt is computed


  //parameter based initialization
  if(dualsUpdateType==0) 
    dualsUpdate = new hiopDualsLsqUpdate(nlp);
  else if(dualsUpdateType==1)
    dualsUpdate = new hiopDualsNewtonLinearUpdate(nlp);
  else assert(false && "dualsUpdateType has an unrecognized value");


  _n_accep_iters = 0;

  _solverStatus = NlpSolve_IncompleteInit;
}

hiopAlgFilterIPM::~hiopAlgFilterIPM()
{
  if(it_curr)  delete it_curr;
  if(it_trial) delete it_trial;
  if(dir)      delete dir;

  if(_c)       delete _c;
  if(_d)       delete _d;
  if(_grad_f)  delete _grad_f;
  if(_Jac_c)   delete _Jac_c;
  if(_Jac_d)   delete _Jac_d;
  if(_Hess)    delete _Hess;

  if(resid)    delete resid;

  if(_c_trial)       delete _c_trial;
  if(_d_trial)       delete _d_trial;
  if(_grad_f_trial)  delete _grad_f_trial;
  if(_Jac_c_trial)   delete _Jac_c_trial;
  if(_Jac_d_trial)   delete _Jac_d_trial;

  if(resid_trial)    delete resid_trial;

  if(logbar) delete logbar;

  if(dualsUpdate) delete dualsUpdate;
}

bool hiopAlgFilterIPM::evalNlp(hiopIterate& iter, 			       
			       double &f, hiopVector& c_, hiopVector& d_, 
			       hiopVector& gradf_,  hiopMatrixDense& Jac_c,  hiopMatrixDense& Jac_d)
{
  bool new_x=true, bret; 
  const hiopVectorPar& it_x = dynamic_cast<const hiopVectorPar&>(*iter.get_x());
  hiopVectorPar 
    &c=dynamic_cast<hiopVectorPar&>(c_), 
    &d=dynamic_cast<hiopVectorPar&>(d_), 
    &gradf=dynamic_cast<hiopVectorPar&>(gradf_);
  const double* x = it_x.local_data_const();//local_data_const();
  //f(x)
  bret = nlp->eval_f(x, new_x, f); assert(bret);
  new_x= false; //same x for the rest
  bret = nlp->eval_grad_f(x, new_x, gradf.local_data());  assert(bret);

  bret = nlp->eval_c     (x, new_x, c.local_data());     assert(bret);
  bret = nlp->eval_d     (x, new_x, d.local_data());     assert(bret);
  bret = nlp->eval_Jac_c (x, new_x, Jac_c.local_data()); assert(bret);
  bret = nlp->eval_Jac_d (x, new_x, Jac_d.local_data()); assert(bret);

  return bret;
}

int hiopAlgFilterIPM::startingProcedure(hiopIterate& it_ini,			       
					double &f, hiopVector& c, hiopVector& d, 
					hiopVector& gradf,  hiopMatrixDense& Jac_c,  hiopMatrixDense& Jac_d)
{
  if(!nlp->get_starting_point(*it_ini.get_x())) {
    nlp->log->printf(hovError, "error: in getting the user provided starting point\n");
    assert(false); return false;
  }

  nlp->runStats.tmSolverInternal.start();
  nlp->runStats.tmStartingPoint.start();

  it_ini.projectPrimalsXIntoBounds(kappa1, kappa2);

  nlp->runStats.tmStartingPoint.stop();
  nlp->runStats.tmSolverInternal.stop();

  this->evalNlp(it_ini, f, c, d, gradf, Jac_c, Jac_d);
  
  nlp->runStats.tmSolverInternal.start();
  nlp->runStats.tmStartingPoint.start();

  it_ini.get_d()->copyFrom(d);

  it_ini.projectPrimalsDIntoBounds(kappa1, kappa2);

  it_ini.determineSlacks();

  it_ini.setBoundsDualsToConstant(1.);



  if(0==dualsInitializ) {
    //LSQ-based initialization of yc and yd

    //is the dualsUpdate already the LSQ-based updater?
    hiopDualsLsqUpdate* updater = dynamic_cast<hiopDualsLsqUpdate*>(dualsUpdate);
    bool deleteUpdater = false;
    if(updater==NULL) {
      updater = new hiopDualsLsqUpdate(nlp);
      deleteUpdater=true;
    }

    //this will update yc and yd in it_ini
    updater->computeInitialDualsEq(it_ini, gradf, Jac_c, Jac_d);

    if(deleteUpdater) delete updater;
  } else {
    it_ini.setEqualityDualsToConstant(0.);
  }

  nlp->log->write("Using initial point:", it_ini, hovIteration);
  //nlp->log->write("Using initial point:", it_ini, hovSummary);
  nlp->runStats.tmStartingPoint.stop();
  nlp->runStats.tmSolverInternal.stop();

  _solverStatus = NlpSolve_SolveNotCalled;

  return true;
}

hiopSolveStatus hiopAlgFilterIPM::run()
{
  nlp->log->printf(hovSummary, "===============\nHiop SOLVER\n===============\n");
  nlp->log->write(NULL, *nlp->options, hovSummary);

#ifdef WITH_MPI
  nlp->log->printf(hovSummary, "Using %d MPI ranks.\n", nlp->get_num_ranks());
#endif  
  nlp->log->write("---------------\nProblem Summary\n---------------", *nlp, hovSummary);

  nlp->runStats.tmOptimizTotal.start();

  startingProcedure(*it_curr, _f_nlp, *_c, *_d, *_grad_f, *_Jac_c, *_Jac_d); //this also evaluates the nlp
  _mu=mu0;


  //update log bar
  logbar->updateWithNlpInfo(*it_curr, _mu, _f_nlp, *_c, *_d, *_grad_f, *_Jac_c, *_Jac_d);
  nlp->log->printf(hovScalars, "log bar obj: %g\n", logbar->f_logbar);
  //recompute the residuals
  resid->update(*it_curr,_f_nlp, *_c, *_d,*_grad_f,*_Jac_c,*_Jac_d, *logbar);

  nlp->log->write("First residual-------------", *resid, hovIteration);
  //nlp->log->printf(hovSummary, "Iter[%d] -> full iterate -------------", iter_num); nlp->log->write("", *it_curr, hovSummary); 

  iter_num=0; nlp->runStats.nIter=iter_num;

  theta_max=1e+4*fmax(1.0,resid->getInfeasNorm());
  theta_min=1e-4*fmax(1.0,resid->getInfeasNorm());
  
  hiopKKTLinSysLowRank* kkt=new hiopKKTLinSysLowRank(nlp);

  _alpha_primal = _alpha_dual = 0;

  // --- Algorithm status 'algStatus ----
  //-1 couldn't solve the problem (most likely because small search step. Restauration phase likely needed)
  // 0 stopped due to tolerances, including acceptable tolerance
  // 1 max iter reached
  // 2 user stop via the iteration callback

  //int algStatus=0; 
  bool bret=true; int lsStatus=-1, lsNum=0;
  _solverStatus = NlpSolve_Pending;
  while(true) {

    bret = evalNlpAndLogErrors(*it_curr, *resid, _mu, 
			       _err_nlp_optim, _err_nlp_feas, _err_nlp_complem, _err_nlp, 
			       _err_log_optim, _err_log_feas, _err_log_complem, _err_log); assert(bret);
    nlp->log->printf(hovScalars, "  Nlp    errs: pr-infeas:%20.14e   dual-infeas:%20.14e  comp:%20.14e  overall:%20.14e\n",
		     _err_nlp_feas, _err_nlp_optim, _err_nlp_complem, _err_nlp);
    nlp->log->printf(hovScalars, "  LogBar errs: pr-infeas:%20.14e   dual-infeas:%20.14e  comp:%20.14e  overall:%20.14e\n",
		     _err_log_feas, _err_log_optim, _err_log_complem, _err_log);
    outputIteration(lsStatus, lsNum);
    //user callback
    if(!nlp->user_callback_iterate(iter_num, _f_nlp, 
				   *it_curr->get_x(),
				   *it_curr->get_zl(),
				   *it_curr->get_zu(),
				   *_c,*_d, 
				   *it_curr->get_yc(),  *it_curr->get_yd(), //lambda,
				   _err_nlp_feas, _err_nlp_optim,
				   _mu,
				   _alpha_dual, _alpha_primal,  lsNum)) {
      _solverStatus = User_Stopped; break;
    }

    /*************************************************
     * Termination check
     ************************************************/
    if(checkTermination(_err_nlp, iter_num, _solverStatus)) {
      break;
    }
    if(NlpSolve_Pending!=_solverStatus) break; //failure of the line search or user stopped. 

    /************************************************
     * update mu and other parameters
     ************************************************/
    while(_err_log<=kappa_eps * _mu) {
      //update mu and tau (fraction-to-boundary)
      bret = updateLogBarrierParameters(*it_curr, _mu, _tau, _mu, _tau);
      if(!bret) break; //no update is necessary
      nlp->log->printf(hovScalars, "Iter[%d] barrier params reduced: mu=%g tau=%g\n", iter_num, _mu, _tau);

      //update only logbar problem  and residual (the NLP didn't change)
      //this->evalNlp(*it_curr, _f_nlp, *_c, *_d, *_grad_f, *_Jac_c, *_Jac_d);
      logbar->updateWithNlpInfo(*it_curr, _mu, _f_nlp, *_c, *_d, *_grad_f, *_Jac_c, *_Jac_d);
      resid->update(*it_curr,_f_nlp, *_c, *_d,*_grad_f,*_Jac_c,*_Jac_d, *logbar); //! should perform only a partial update since NLP didn't change
      bret = evalNlpAndLogErrors(*it_curr, *resid, _mu, 
				 _err_nlp_optim, _err_nlp_feas, _err_nlp_complem, _err_nlp, 
				 _err_log_optim, _err_log_feas, _err_log_complem, _err_log); assert(bret);
      nlp->log->printf(hovScalars, "  Nlp    errs: pr-infeas:%20.14e   dual-infeas:%20.14e  comp:%20.14e  overall:%20.14e\n",
		       _err_nlp_feas, _err_nlp_optim, _err_nlp_complem, _err_nlp);
      nlp->log->printf(hovScalars, "  LogBar errs: pr-infeas:%20.14e   dual-infeas:%20.14e  comp:%20.14e  overall:%20.14e\n",
		       _err_log_feas, _err_log_optim, _err_log_complem, _err_log);    
      
      filter.reinitialize(theta_max);
      //recheck residuals at the first iteration in case the starting pt is  very good
      //if(iter_num==0) {
      //	continue; 
      //}
    }
    nlp->log->printf(hovScalars, "Iter[%d] logbarObj=%20.14e (mu=%12.5e)\n", iter_num, logbar->f_logbar,_mu);
    /****************************************************
     * Search direction calculation
     ***************************************************/
    //first update the Hessian and kkt system
    _Hess->update(*it_curr,*_grad_f,*_Jac_c,*_Jac_d);
    kkt->update(it_curr,_grad_f,_Jac_c,_Jac_d, _Hess);
    //nlp->log->write("resid ppp -----------------", *resid, hovScalars);
    bret = kkt->computeDirections(resid,dir); assert(bret==true);


    //nlp->log->printf(hovIteration, "Iter[%d] full search direction -------------\n", iter_num); nlp->log->write("", *dir, hovIteration);
    //nlp->log->printf(hovScalars, "Iter[%d] full search direction -------------\n", iter_num); nlp->log->write("", *dir, hovScalars);
    /***************************************************************
     * backtracking line search
     ****************************************************************/
    nlp->runStats.tmSolverInternal.start();

    //maximum  step
    bret = it_curr->fractionToTheBdry(*dir, _tau, _alpha_primal, _alpha_dual); assert(bret);
    double theta = resid->getInfeasNorm(); //at it_curr
    double theta_trial;
    nlp->runStats.tmSolverInternal.stop();

    //lsStatus: line search status for the accepted trial point. Needed to update the filter
    //-1 uninitialized (first iteration)
    //0 unsuccessful (small step size)
    //1 "sufficient decrease" when far away from solution (theta_trial>theta_min)
    //2 close to solution but switching condition does not hold, so trial accepted based on "sufficient decrease"
    //3 close to solution and switching condition is true; trial accepted based on Armijo
    lsStatus=0; lsNum=0;

    bool grad_phi_dx_computed=false; double grad_phi_dx;
    double infeas_nrm_trial=-1.; //this will cache the primal infeasibility norm for (reuse)use in the dual updating
    //this is the linesearch loop
    while(true) {
      nlp->runStats.tmSolverInternal.start(); //---

      //check the step against the minimum step size
      if(_alpha_primal<1e-16) {
	nlp->log->write("Panic: minimum step size reached. The problem may be infeasible or the gradient inaccurate. Will exit here.",hovError);
	_solverStatus = Steplength_Too_Small;
	break;
      }
      bret = it_trial->takeStep_primals(*it_curr, *dir, _alpha_primal, _alpha_dual); assert(bret);
      nlp->runStats.tmSolverInternal.stop(); //---

      //evaluate the problem at the trial iterate (functions only)
      this->evalNlp_funcOnly(*it_trial, _f_nlp_trial, *_c_trial, *_d_trial);
      logbar->updateWithNlpInfo_trial_funcOnly(*it_trial, _f_nlp_trial, *_c_trial, *_d_trial);

      nlp->runStats.tmSolverInternal.start(); //---
      //compute infeasibility theta at trial point.
      infeas_nrm_trial = theta_trial = resid->computeNlpInfeasNorm(*it_trial, *_c_trial, *_d_trial);

      lsNum++;

      nlp->log->printf(hovLinesearch, "  trial point %d: alphaPrimal=%14.8e barier:(%22.16e)>%15.9e theta:(%22.16e)>%22.16e\n",
		       lsNum, _alpha_primal, logbar->f_logbar, logbar->f_logbar_trial, theta, theta_trial);

      //let's do the cheap, "sufficient progress" test first, before more involved/expensive tests. 
      // This simple test is good enough when iterate is far away from solution
      if(theta>=theta_min) {
	//check the filter and the sufficient decrease condition (18)
	if(!filter.contains(theta_trial,logbar->f_logbar_trial)) {
	  if(theta_trial<=(1-gamma_theta)*theta || logbar->f_logbar_trial<=logbar->f_logbar - gamma_phi*theta) {
	    //trial good to go
	    nlp->log->printf(hovLinesearchVerb, "Linesearch: accepting based on suff. decrease (far from solution)\n");
	    lsStatus=1;
	    break;
	  } else {
	    //there is no sufficient progress 
	    _alpha_primal *= 0.5;
	    continue;
	  }
	} else {
	  //it is in the filter 
	  _alpha_primal *= 0.5;
	  continue;
	}  
	nlp->log->write("Warning (close to panic): I got to a point where I wasn't supposed to be. (1)", hovWarning);
      } else {
	// if(theta<theta_min,  then check the switching condition and, if true, rely on Armijo rule.
	// first compute grad_phi^T d_x if it hasn't already been computed
	if(!grad_phi_dx_computed) { 
	  nlp->runStats.tmSolverInternal.stop(); //---
	  grad_phi_dx = logbar->directionalDerivative(*dir); 
	  grad_phi_dx_computed=true; 
	  nlp->runStats.tmSolverInternal.start(); //---
	}
	nlp->log->printf(hovLinesearch, "Linesearch: grad_phi_dx = %22.15e\n", grad_phi_dx);
	//nlp->log->printf(hovSummary, "Linesearch: grad_phi_dx = %22.15e      %22.15e >   %22.15e  \n", grad_phi_dx, _alpha_primal*pow(-grad_phi_dx,s_phi), delta*pow(theta,s_theta));
	//nlp->log->printf(hovSummary, "Linesearch: s_phi=%22.15e;   s_theta=%22.15e; theta=%22.15e; delta=%22.15e \n", s_phi, s_theta, theta, delta);
	//this is the actual switching condition
	if(grad_phi_dx<0 && _alpha_primal*pow(-grad_phi_dx,s_phi)>delta*pow(theta,s_theta)) {

	  if(logbar->f_logbar_trial <= logbar->f_logbar + eta_phi*_alpha_primal*grad_phi_dx) {
	    lsStatus=3;
	    nlp->log->printf(hovLinesearchVerb, "Linesearch: accepting based on Armijo (switch cond also passed)\n");
	    break; //iterate good to go since it satisfies Armijo
	  } else {  //Armijo is not satisfied
	    _alpha_primal *= 0.5; //reduce step and try again
	    continue;
	  }
	} else {//switching condition does not hold  
	  
	  //ok to go with  "sufficient progress" condition even when close to solution, provided the switching condition is not satisfied
	  //check the filter and the sufficient decrease condition (18)
	  if(!filter.contains(theta_trial,logbar->f_logbar_trial)) {
	    if(theta_trial<=(1-gamma_theta)*theta || logbar->f_logbar_trial<=logbar->f_logbar - gamma_phi*theta) {
	    //if(logbar->f_logbar_trial<=logbar->f_logbar - gamma_phi*theta) {
	      //trial good to go
	      nlp->log->printf(hovLinesearchVerb, "Linesearch: accepting based on suff. decrease (switch cond also passed)\n");
	      lsStatus=2;
	      break;
	    } else {
	      //there is no sufficient progress 
	      _alpha_primal *= 0.5;
	      continue;
	    }
	  } else {
	    //it is in the filter 
	    _alpha_primal *= 0.5;
	    continue;
	  } 
	} // end of else: switching condition does not hold

	nlp->log->write("Warning (close to panic): I got to a point where I wasn't supposed to be. (2)", hovWarning);

      } //end of else: theta_trial<theta_min
    } //end of while for the linesearch loop
    nlp->runStats.tmSolverInternal.stop();

    //post line-search stuff  
    //filter is augmented whenever the switching condition or Armijo rule do not hold for the trial point that was just accepted
    if(lsStatus==1) {
      //need to check switching cond and Armijo to decide if filter is augmented
      if(!grad_phi_dx_computed) { grad_phi_dx = logbar->directionalDerivative(*dir); grad_phi_dx_computed=true; }
      
      //this is the actual switching condition
      if(grad_phi_dx<0 && _alpha_primal*pow(-grad_phi_dx,s_phi)>delta*pow(theta,s_theta)) {
	//check armijo
	if(logbar->f_logbar_trial <= logbar->f_logbar + eta_phi*_alpha_primal*grad_phi_dx) {
	  //filter does not change
	} else {
	  //Armijo does not hold
	  filter.add(logbar->f_logbar_trial, theta_trial);
	}
      } else { //switching condition does not hold
	filter.add(logbar->f_logbar_trial, theta_trial);
      }

    } else if(lsStatus==2) {
      //switching condition does not hold for the trial
      filter.add(logbar->f_logbar_trial, theta_trial);
    } else if(lsStatus==3) {
      //Armijo (and switching condition) hold, nothing to do.
    } else if(lsStatus==0) {
      //small step; take the update; if the update doesn't pass the convergence test, the optimiz. loop will exit.
    } else 
      assert(false && "unrecognized value for lsStatus");

    nlp->log->printf(hovScalars, "Iter[%d] -> accepted step primal=[%17.11e] dual=[%17.11e]\n", iter_num, _alpha_primal, _alpha_dual);
    iter_num++; nlp->runStats.nIter=iter_num;

    //evaluate derivatives at the trial (and to be accepted) trial point
    this->evalNlp_derivOnly(*it_trial, *_grad_f, *_Jac_c, *_Jac_d);

    nlp->runStats.tmSolverInternal.start(); //-----
    //reuse function values
    _f_nlp=_f_nlp_trial; hiopVector* pvec=_c_trial; _c_trial=_c; _c=pvec; pvec=_d_trial; _d_trial=_d; _d=pvec;

    //update and adjust the duals
    //it_trial->takeStep_duals(*it_curr, *dir, _alpha_primal, _alpha_dual); assert(bret);
    //bret = it_trial->adjustDuals_primalLogHessian(_mu,kappa_Sigma); assert(bret);
    assert(infeas_nrm_trial>=0 && "this should not happen");
    bret = dualsUpdate->go(*it_curr, *it_trial, 
			   _f_nlp, *_c, *_d, *_grad_f, *_Jac_c, *_Jac_d, *dir,  
			   _alpha_primal, _alpha_dual, _mu, kappa_Sigma, infeas_nrm_trial); assert(bret);

    //update current iterate (do a fast swap of the pointers)
    hiopIterate* pit=it_curr; it_curr=it_trial; it_trial=pit;
    nlp->log->printf(hovIteration, "Iter[%d] -> full iterate:", iter_num); nlp->log->write("", *it_curr, hovIteration); 
    nlp->runStats.tmSolverInternal.stop(); //-----

    //notify logbar about the changes
    logbar->updateWithNlpInfo(*it_curr, _mu, _f_nlp, *_c, *_d, *_grad_f, *_Jac_c, *_Jac_d);
    //update residual
    resid->update(*it_curr,_f_nlp, *_c, *_d,*_grad_f,*_Jac_c,*_Jac_d, *logbar);
    nlp->log->printf(hovIteration, "Iter[%d] full residual:-------------\n", iter_num); nlp->log->write("", *resid, hovIteration);
  }

  nlp->runStats.tmOptimizTotal.stop();

  //_solverStatus contains the termination information
  displayTerminationMsg();

  //user callback
  nlp->user_callback_solution(_solverStatus,
			      *it_curr->get_x(),
			      *it_curr->get_zl(),
			      *it_curr->get_zu(),
			      *_c,*_d, 
			      *it_curr->get_yc(),  *it_curr->get_yd(),
			      _f_nlp);
  delete kkt;

  return _solverStatus;
}


bool hiopAlgFilterIPM::
checkTermination(const double& err_nlp, const int& iter_num, hiopSolveStatus& status)
{
  if(err_nlp<=eps_tol)   { _solverStatus = Solve_Success;     return true; }
  if(iter_num>=max_n_it) { _solverStatus = Max_Iter_Exceeded; return true; }

  if(err_nlp<=eps_tol_accep) _n_accep_iters++;
  else _n_accep_iters = 0;

  if(_n_accep_iters>=accep_n_it) { _solverStatus = Solve_Acceptable_Level; return true; }

  return false;
}

/***** Termination message *****/
void hiopAlgFilterIPM::displayTerminationMsg() {

  switch(_solverStatus) {
  case Solve_Success: 
    {
      nlp->log->printf(hovSummary, "Successfull termination.\n%s\n", nlp->runStats.getSummary().c_str());
      break;
    }
  case Solve_Acceptable_Level:
    {
      nlp->log->printf(hovSummary, "Solve to only to the acceptable tolerance(s).\n%s\n", nlp->runStats.getSummary().c_str());
      break;
    }
  case Max_Iter_Exceeded:
    {
      nlp->log->printf(hovSummary, "Maximum number of iterations reached.\n%s\n", nlp->runStats.getSummary().c_str());
      break;
    }
  case Steplength_Too_Small:
    {
      nlp->log->printf(hovSummary, "Couldn't solve the problem.\n%s\n", nlp->runStats.getSummary().c_str());
      nlp->log->printf(hovSummary, "Linesearch returned unsuccessfully (small step). Probable cause: inaccurate gradients/Jacobians or infeasible problem.\n");
      break;
    }
  case User_Stopped:
    {
      nlp->log->printf(hovSummary, "Stopped by the user through the user provided iterate callback.\n%s\n", nlp->runStats.getSummary().c_str());
      break;
    }
  default:
    {
      nlp->log->printf(hovSummary, "Do not know why hiop stopped. This shouldn't happen. :)\n%s\n", nlp->runStats.getSummary().c_str());
      assert(false && "Do not know why hiop stopped. This shouldn't happen.");
      break;
    }
  };
}

bool hiopAlgFilterIPM::
updateLogBarrierParameters(const hiopIterate& it, const double& mu_curr, const double& tau_curr,
			   double& mu_new, double& tau_new)
{
  double new_mu = fmax(eps_tol/10, fmin(kappa_mu*mu_curr, pow(mu_curr,theta_mu)));
  if(fabs(new_mu-mu_curr)<1e-16) return false;
  mu_new  = new_mu;
  tau_new = fmax(tau_min,1.0-mu_new);
  return true;
}

double hiopAlgFilterIPM::thetaLogBarrier(const hiopIterate& it, const hiopResidual& resid, const double& mu)
{
  //actual nlp errors
  double optim, feas, complem;
  resid.getNlpErrors(optim, feas, complem);
  return feas;
}


bool hiopAlgFilterIPM::
evalNlpAndLogErrors(const hiopIterate& it, const hiopResidual& resid, const double& mu,
		    double& nlpoptim, double& nlpfeas, double& nlpcomplem, double& nlpoverall,
		    double& logoptim, double& logfeas, double& logcomplem, double& logoverall)
{
  nlp->runStats.tmSolverInternal.start();

  long long n=nlp->n_complem(), m=nlp->m();
  //the "total" norms of the duals
  //  nrmDualEqu  = ( ||yc||_inf + ||yd||_inf )   
  //  nrmDualBou  = ( ||zl||_H + ||zu||_H + ||vl||_inf + ||vu||_inf )  
  double nrmDualBou, nrmDualEqu;
  it.totalNormOfDuals(nrmDualEqu, nrmDualBou);

  //scaling factors
  double sd = fmax(p_smax,(nrmDualBou/4+nrmDualEqu/2)/(n+m)) / p_smax;
  double sc = n==0?0:fmax(p_smax,nrmDualBou/4) / p_smax;
  //actual nlp errors 
  resid.getNlpErrors(nlpoptim, nlpfeas, nlpcomplem);

  //finally, the scaled nlp error
  nlpoverall = fmax(nlpoptim/sd, fmax(nlpfeas, nlpcomplem/sc));

  //actual log errors
  resid.getBarrierErrors(logoptim, logfeas, logcomplem);

  //finally, the scaled barrier error
  logoverall = fmax(logoptim/sd, fmax(logfeas, logcomplem/sc));
  nlp->runStats.tmSolverInternal.start();
  return true;
}




bool hiopAlgFilterIPM::evalNlp_funcOnly(hiopIterate& iter,
					double& f, hiopVector& c_, hiopVector& d_)
{
  bool new_x=true, bret; 
  const hiopVectorPar& it_x = dynamic_cast<const hiopVectorPar&>(*iter.get_x());
  hiopVectorPar 
    &c=dynamic_cast<hiopVectorPar&>(c_), 
    &d=dynamic_cast<hiopVectorPar&>(d_);
  const double* x = it_x.local_data_const();
  bret = nlp->eval_f(x, new_x, f); assert(bret);
  new_x= false; //same x for the rest
  bret = nlp->eval_c(x, new_x, c.local_data());     assert(bret);
  bret = nlp->eval_d(x, new_x, d.local_data());     assert(bret);
  return bret;
}
bool hiopAlgFilterIPM::evalNlp_derivOnly(hiopIterate& iter,
					 hiopVector& gradf_,  hiopMatrixDense& Jac_c,  hiopMatrixDense& Jac_d)
{
  bool new_x=false; //functions were previously evaluated in the line search
  bool bret;
  const hiopVectorPar& it_x = dynamic_cast<const hiopVectorPar&>(*iter.get_x());
  hiopVectorPar & gradf=dynamic_cast<hiopVectorPar&>(gradf_);
  const double* x = it_x.local_data_const();
  bret = nlp->eval_grad_f(x, new_x, gradf.local_data()); assert(bret);
  bret = nlp->eval_Jac_c (x, new_x, Jac_c.local_data()); assert(bret);
  bret = nlp->eval_Jac_d (x, new_x, Jac_d.local_data()); assert(bret);
  return bret;
}

void hiopAlgFilterIPM::outputIteration(int lsStatus, int lsNum)
{
  if(iter_num/10*10==iter_num) 
    nlp->log->printf(hovSummary, "iter    objective     inf_pr     inf_du   lg(mu)  alpha_du   alpha_pr linesrch\n");

  if(lsStatus==-1) 
    nlp->log->printf(hovSummary, "%4d %14.7e %7.3e  %7.3e %6.2f  %7.3e  %7.3e  -(-)\n",
		     iter_num, _f_nlp, _err_nlp_feas, _err_nlp_optim, log10(_mu), _alpha_dual, _alpha_primal); 
  else {
    char stepType[2];
    if(lsStatus==1) strcpy(stepType, "s");
    else if(lsStatus==2) strcpy(stepType, "h");
    else if(lsStatus==3) strcpy(stepType, "f");
    else strcpy(stepType, "?");
    nlp->log->printf(hovSummary, "%4d %14.7e %7.3e  %7.3e %6.2f  %7.3e  %7.3e  %d(%s)\n",
		     iter_num, _f_nlp, _err_nlp_feas, _err_nlp_optim, log10(_mu), _alpha_dual, _alpha_primal, lsNum, stepType); 
  }
}

/* returns the objective value; valid only after 'run' method has been called */
double hiopAlgFilterIPM::getObjective() const
{
  if(_solverStatus==NlpSolve_IncompleteInit || _solverStatus == NlpSolve_SolveNotCalled)
    nlp->log->printf(hovError, "getObjective: hiOp did not initialize entirely or the 'run' function was not called.");
  if(_solverStatus==NlpSolve_Pending)
    nlp->log->printf(hovWarning, "getObjective: hiOp does not seem to have completed yet. The objective value returned may not be optimal.");
  return _f_nlp;
}
  /* returns the primal vector x; valid only after 'run' method has been called */
void hiopAlgFilterIPM::getSolution(const double* x) const
{
  if(_solverStatus==NlpSolve_IncompleteInit || _solverStatus == NlpSolve_SolveNotCalled)
    nlp->log->printf(hovError, "getSolution: hiOp did not initialize entirely or the 'run' function was not called.");
  if(_solverStatus==NlpSolve_Pending)
    nlp->log->printf(hovWarning, "getSolution: hiOp does not seem to have completed yet. The primal vector returned may not be optimal.");
}

hiopSolveStatus hiopAlgFilterIPM::getSolveStatus() const
{
  return _solverStatus;
}

/**** implementation of hiopAlgFilterFiniteDimIPM ****/

bool hiopAlgFilterFiniteDimIPM::
evalNlpAndLogErrors(const hiopIterate& it, const hiopResidual& resid, const double& mu,
		    double& nlpoptim, double& nlpfeas, double& nlpcomplem, double& nlpoverall,
		    double& logoptim, double& logfeas, double& logcomplem, double& logoverall)
{
  nlp->runStats.tmSolverInternal.start();

  long long n=nlp->n_complem(), m=nlp->m();
  //the one norms
  double nrmDualBou, nrmDualEqu;
  it.normOneOfDuals(nrmDualEqu, nrmDualBou);

  //scaling factors
  double sd = fmax(p_smax,(nrmDualBou+nrmDualEqu)/(n+m)) / p_smax;
  double sc = n==0?0:fmax(p_smax,nrmDualBou/n) / p_smax;
  //actual nlp errors 
  resid.getNlpErrors(nlpoptim, nlpfeas, nlpcomplem);

  printf("child class: %g %g %g\n", nlpoptim, nlpfeas, nlpcomplem);

  //finally, the scaled nlp error
  nlpoverall = fmax(nlpoptim/sd, fmax(nlpfeas, nlpcomplem/sc));

  //actual log errors
  resid.getBarrierErrors(logoptim, logfeas, logcomplem);

  //finally, the scaled barrier error
  logoverall = fmax(logoptim/sd, fmax(logfeas, logcomplem/sc));
  nlp->runStats.tmSolverInternal.start();
  return true;
}
}
