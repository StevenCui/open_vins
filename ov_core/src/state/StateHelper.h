//
// Created by keck on 6/4/19.
//

#ifndef OPEN_VINS_STATEHELPER_H
#define OPEN_VINS_STATEHELPER_H

/** @brief Class where all the static functions used to manipulate the covariance are implemented
 */

#include "State.h"

class StateHelper{

public:

    /**
    * @brief Clones "variable to clone" and places it at end of covariance
    * @param state Pointer to state
    * @param variable_to_clone Pointer to variable that will be cloned
    */
    static Type* clone(State* state, Type* variable_to_clone) {

        //Get total size of new cloned variables, and the old covariance size
        int total_size = variable_to_clone->size();
        int old_size = (int)state->Cov().rows();
        int new_loc = (int)state->Cov().rows();

        // Resize both our covariance to the new size
        state->Cov().conservativeResizeLike(Eigen::MatrixXd::Zero(state->Cov().rows()+total_size,state->Cov().rows()+total_size));

        // What is the new state, and variable we inserted
        const std::vector<Type*> new_variables = state->variables();
        Type* new_clone = nullptr;

        // Loop through all variables, and find the variable that we are going to clone
        for (size_t k=0; k < state->variables().size(); k++){

            // Skip this if it is not the same
            Type* type_check = state->variables()[k]->check_if_same_variable(variable_to_clone);
            if (type_check == nullptr)
                continue;

            //So we will clone this one
            int old_loc = type_check->id();

            // Copy the covariance elements
            state->Cov().block(new_loc,new_loc,total_size,total_size) = state->Cov().block(old_loc,old_loc,total_size,total_size);
            state->Cov().block(0,new_loc,old_size,total_size) = state->Cov().block(0,old_loc,old_size,total_size);
            state->Cov().block(new_loc,0,total_size,old_size) = state->Cov().block(old_loc,0,total_size,old_size);

            //Create clone from the type being cloned
            new_clone = type_check->clone();
            new_clone->set_local_id(new_loc);

            //Add to variable list
            state->insert_variable(new_clone);
            break;

        }

        // Check if the current state has the GPS enabled
        if(new_clone == nullptr) {
            std::cerr << "CovManager::clone() - Called on variable is not in the state" << std::endl;
            std::cerr << "CovManager::clone() - Ensure that the variable specified is a variable, or sub-variable.." << std::endl;
            std::exit(EXIT_FAILURE);
        }

        return new_clone;

    }


    /** @brief Performs EKF Update
     *  @param state Pointer to state
     *  @param H_order Variable ordering used in the compressed Jacobian
     *  @param H Condensed Jacobian of updating measurement
     *  @param res residual of updating measurement
     *  @param R updating measurement covariance
     *
     */
    static void EKFUpdate(State* state, const std::vector<Type*> &H_order, const Eigen::MatrixXd &H,
                   const Eigen::VectorXd &res, const Eigen::MatrixXd &R){

        //==========================================================
        //==========================================================
        // Part of the Kalman Gain K = M*S^{-1}

        assert(res.rows() == R.rows());
        assert(H.rows() == res.rows());


        Eigen::MatrixXd M_a = Eigen::MatrixXd::Zero(state->Cov().rows(), res.rows());

        std::vector <int> H_id;
        std::vector <bool> H_is_active;
        int current_it=0;

        // Get the location in small jacobian for each measuring variable
        for (Type* meas_var: H_order){
            H_id.push_back(current_it);
            current_it+=meas_var->size();
        }

        auto Cov = state->Cov();

        //==========================================================
        //==========================================================
        // For each active variable find its M = P*H^T
        for (Type* var: state->variables()) {
            // Sum up effect of each subjacobian= K_i= \sum_m (P_im Hm^T)
            Eigen::MatrixXd M_i = Eigen::MatrixXd::Zero(var->size(), res.rows());
            for (size_t i = 0; i < H_order.size(); i++) {
                Type *meas_var = H_order[i];
                M_i.noalias() += Cov.block(var->id(), meas_var->id(), var->size(), meas_var->size()) *
                        H.block(0, H_id[i], H.rows(), meas_var->size()).transpose();
            }
            M_a.block(var->id(), 0, var->size(), res.rows()) = M_i;
        }

        //==========================================================
        //==========================================================
        //Get S
        Eigen::MatrixXd P_small = state->get_marginal_covariance(H_order);

        // S = H*Cov*H' + R
        Eigen::MatrixXd S(R.rows(), R.rows());
        S.triangularView<Eigen::Upper>() = H*P_small*H.transpose();
        S.triangularView<Eigen::Upper>() += R;
        //S = 0.5*(S+S.transpose());

        // Inverse our S (should we use a more stable method here??)
        Eigen::MatrixXd Sinv = Eigen::MatrixXd::Identity(R.rows(), R.rows());
        S.selfadjointView<Eigen::Upper>().llt().solveInPlace(Sinv);
        Eigen::MatrixXd K = M_a*Sinv.selfadjointView<Eigen::Upper>();

        // Update Covariance
        Cov.triangularView<Eigen::Upper>() -= K*M_a.transpose();
        Cov = Cov.selfadjointView<Eigen::Upper>();

        // Calculate our delta and pass it to update all our state variables
        state->update(K*res);

    }


    /**
    * @brief Initializes new variable into covariance
    * @param state Pointer to state
    * @param new_variable Pointer to variable to be initialized
    * @param H_order Vector of pointers in order they are contained in the condensed state Jacobian
    * @param H_R Jacobian of initializing measurements wrt variables in H_order
    * @param H_L Jacobian of initializing measurements wrt new variable
    * @param R Covariance of initializing measurements
    * @param res Residual of initializing measurements
    */
    static void invertible_initialize(State* state, Type* new_variable, const std::vector<Type*> &H_order, const Eigen::MatrixXd &H_R,
                                           const Eigen::MatrixXd &H_L, const Eigen::MatrixXd &R, const Eigen::VectorXd &res){

        //==========================================================
        //==========================================================
        // Part of the Kalman Gain K = M*S^{-1}

        assert(H_L.rows() == H_L.cols());
        assert(new_variable->size() == H_L.rows());

        Eigen::MatrixXd H_Linv = H_L.inverse();

        Eigen::MatrixXd M_a = Eigen::MatrixXd::Zero(state->Cov().rows(), res.rows());

        std::vector <int> H_id;
        std::vector <bool> H_is_active;
        int current_it=0;

        // Get the location in small jacobian for each measuring variable
        for (Type* meas_var: H_order){
            H_id.push_back(current_it);
            current_it+=meas_var->size();
        }

        //==========================================================
        //==========================================================
        // For each active variable find its M = P*H^T
        for (Type* var: state->variables()) {
            // Sum up effect of each subjacobian= K_i= \sum_m (P_im Hm^T)
            Eigen::MatrixXd M_i = Eigen::MatrixXd::Zero(var->size(), res.rows());
            for (size_t i = 0; i < H_order.size(); i++) {
                Type *meas_var = H_order[i];
                M_i += state->Cov().block(var->id(), meas_var->id(), var->size(), meas_var->size()) *
                        H_R.block(0, H_id[i], H_R.rows(), meas_var->size()).transpose();
            }
            M_a.block(var->id(), 0, var->size(), res.rows()) = M_i;
        }


        //==========================================================
        //==========================================================
        //Get covariance of this small jacobian
        Eigen::MatrixXd P_small= state->get_marginal_covariance(H_order);

        // M = H_R*Cov*H_R' + R
        Eigen::MatrixXd M(H_R.rows(), H_R.rows());
        M.triangularView<Eigen::Upper>() = H_R*P_small*H_R.transpose();
        M.triangularView<Eigen::Upper>() += R;

        // Covariance of the variable/landmark that will be initialized
        Eigen::MatrixXd P_LL = H_Linv*M.selfadjointView<Eigen::Upper>()*H_Linv.transpose();

        size_t oldSize = state->Cov().rows();

        state->Cov().conservativeResizeLike(Eigen::MatrixXd::Zero(state->Cov().rows()+new_variable->size(),
                state->Cov().rows()+new_variable->size()));

        state->Cov().block(0, oldSize, oldSize, new_variable->size()).noalias() = -M_a*H_Linv.transpose();
        state->Cov().block(oldSize, 0, new_variable->size(), oldSize) = state->Cov().block(0, oldSize, oldSize, new_variable->size()).transpose();
        state->Cov().block(oldSize, oldSize, new_variable->size(), new_variable->size()) = P_LL;


        // Update the variable that will be initialized (invertible systems can only update the new variable. However this
        // Update should be almost zero if we already used a conditional Gauss-Newton to solve for the initial estimate
        new_variable->update(H_Linv*res);

        // Now collect results, and add it to the state variables
        new_variable->set_local_id((int)(state->Cov().rows()-new_variable->size()));
        state->insert_variable(new_variable);


    }

    /**
    * @brief Initializes new variable into covariance. Uses Givens to separate into updating and
    * initializing systems (therefore system must be fed as isotropic)
    * @param state Pointer to state
    * @param new_variable Pointer to variable to be initialized
    * @param H_order Vector of pointers in order they are contained in the condensed state Jacobian
    * @param H_R Jacobian of initializing measurements wrt variables in H_order
    * @param H_L Jacobian of initializing measurements wrt new variable
    * @param R Covariance of initializing measurements
    * @param res Residual of initializing measurements
    */
    static void initialize(State* state, Type* new_variable, const std::vector<Type*> &H_order, Eigen::MatrixXd &H_R,
                                      Eigen::MatrixXd &H_L, Eigen::MatrixXd &R, Eigen::VectorXd &res){

        //==========================================================
        //==========================================================
        // Part of the Kalman Gain K = M*S^{-1}

        size_t new_var_size = new_variable->size();
        assert (new_var_size == H_L.cols());

        Eigen::JacobiRotation<double> tempHo_GR;
        for (int n = 0; n < H_L.cols(); ++n) {
            for (int m = (int) H_L.rows() - 1; m > n; m--) {
                // Givens matrix G
                tempHo_GR.makeGivens(H_L(m - 1, n), H_L(m, n));
                // Multiply G to the corresponding lines (m-1,m) in each matrix
                // Note: we only apply G to the nonzero cols [n:Ho.cols()-n-1], while
                //       it is equivalent to applying G to the entire cols [0:Ho.cols()-1].
                (H_L.block(m - 1, n, 2, H_L.cols() - n)).applyOnTheLeft(0, 1, tempHo_GR.adjoint());
                (res.block(m - 1, 0, 2, 1)).applyOnTheLeft(0, 1, tempHo_GR.adjoint());
                (H_R.block(m - 1, 0, 2, H_R.cols())).applyOnTheLeft(0, 1, tempHo_GR.adjoint());
            }
        }


        //Separate into initializing and updating portions
        Eigen::MatrixXd Hxinit = H_R.block(0, 0, new_var_size, H_R.cols());
        Eigen::MatrixXd Hup = H_R.block(new_var_size, 0, H_R.rows() - new_var_size, H_R.cols());

        Eigen::MatrixXd H_finit = H_L.block(0, 0, new_var_size, new_var_size);

        Eigen::VectorXd resinit = res.block(0, 0, new_var_size, 1);
        Eigen::VectorXd resup = res.block(new_var_size, 0, res.rows() - new_var_size, 1);

        Eigen::MatrixXd Rinit = R.block(0,0,new_var_size, new_var_size);
        Eigen::MatrixXd Rup = R.block(new_var_size,new_var_size, R.rows()- new_var_size,  R.rows() - new_var_size);

        //===========================================
        // Finally, initialize it in our state
        invertible_initialize(state, new_variable, H_order, Hxinit, H_finit,
                                          Rinit, resinit);

        //Update with updating portion
        if (Hup.rows() > 0) {
            StateHelper::EKFUpdate(state, H_order, Hup, resup, Rup);
        }


    }

    /**
     *
     * @param state Pointer to state
     * @param last_w The estimated angular velocity at cloning time (used to estimate imu-cam time offset)
     */

    static void augment_clone(State* state, Eigen::Matrix<double,3,1> last_w) {

        auto imu = state->imu();
        auto Cov = state->Cov();

        // Call on our marginalizer to clone, it will add it to our vector of types
        // NOTE: this will clone the clone pose to the END of the covariance...
        Type* posetemp = StateHelper::clone(state, imu->pose());

        // Cast to a JPL pose type
        PoseJPL* pose = dynamic_cast<PoseJPL*>(posetemp);

        // Check that it was a valid cast
        if(pose == nullptr) {
            ROS_ERROR("INVALID OBJECT RETURNED FROM MARGINALIZER, EXITING!#!@#!@#");
            exit(EXIT_FAILURE);
        }

        // Append the new clone to our clone vector
        state->insert_clone(state->timestamp(), pose);

        // If we are doing time calibration, then our clones are a function of the time offset
        // Logic is based on Mingyang Li and Anastasios I. Mourikis paper:
        // http://journals.sagepub.com/doi/pdf/10.1177/0278364913515286
        if (state->options().do_calib_camera_timeoffset){
            // Jacobian to augment by
            Eigen::Matrix<double,6,1> dnc_dt = Eigen::MatrixXd::Zero(6,1);
            dnc_dt.block(0,0,3,1)= last_w;
            dnc_dt.block(3,0,3,1)= imu->vel();
            // Augment covariance with time offset Jacobian
            Cov.block(0,pose->id(),Cov.rows(),6) +=  Cov.block(0,state->calib_dt_CAMtoIMU()->id(),
                    Cov.rows(),1)*dnc_dt.transpose();
            Cov.block(pose->id(),0,6,Cov.rows()) += dnc_dt*Cov.block(state->calib_dt_CAMtoIMU()->id(),0,1,Cov.rows());
            Cov.block(pose->id(),pose->id(),6,6) += dnc_dt*Cov(state->calib_dt_CAMtoIMU()->id(),
                    state->calib_dt_CAMtoIMU()->id())*dnc_dt.transpose();
        }

    }




};



#endif //OPEN_VINS_STATEHELPER_H
