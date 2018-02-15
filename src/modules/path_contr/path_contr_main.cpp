/****************************************************************************
 *
 *   Copyright (c) 2013-2015 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file path_contr_main.cpp
 * Hippocampus path controller.
 *
 * Publication for the desired attitude tracking:
 * Daniel Mellinger and Vijay Kumar. Minimum Snap Trajectory Generation and Control for Quadrotors.
 * Int. Conf. on Robotics and Automation, Shanghai, China, May 2011.
 *
 * based on mc_att_control_main.cpp from
 * @author Lorenz Meier		<lorenz@px4.io>
 * @author Anton Babushkin	<anton.babushkin@me.com>
 * @author Sander Smeets	<sander@droneslab.com>
 *
 * adjusted by
 * @author Nils Rottmann    <Nils.Rottmann@tuhh.de>
 *
 * The controller has two loops: P loop for position and angular error and PD loop for velocity and angular rate error.
 */


#include <px4_config.h>
#include <px4_defines.h>
#include <px4_tasks.h>
#include <px4_posix.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <drivers/drv_hrt.h>
#include <arch/board/board.h>
// uORB topics
#include <uORB/uORB.h>
#include <uORB/topics/actuator_controls.h>              // this topic gives the actuators control input
#include <uORB/topics/actuator_outputs.h>              // this topic gives the actuators control output
#include <uORB/topics/vehicle_attitude.h>               // orientation data
#include <uORB/topics/vehicle_local_position.h>         // position data
#include <uORB/topics/trajectory_setpoint.h>
#include <uORB/topics/parameter_update.h>
#include <uORB/topics/logging_hippocampus.h>            // logging message to have everything in one at the same time steps
#include <uORB/topics/pressure.h>
// system libraries
#include <systemlib/param/param.h>
#include <systemlib/err.h>
#include <systemlib/perf_counter.h>
#include <systemlib/systemlib.h>
#include <systemlib/circuit_breaker.h>
// internal libraries
#include <lib/mathlib/mathlib.h>
#include <lib/geo/geo.h>
#include <lib/tailsitter_recovery/tailsitter_recovery.h>
#include <uORB/topics/debug_value.h>
#include <uORB/topics/debug_vect.h>
#include <uORB/topics/sensor_combined.h>


// Hippocampus path controller
extern "C" __EXPORT int path_contr_main(int argc, char *argv[]);

// the class from which an instance will be initiated by starting this application
class HippocampusPathControl
{
public:
	// Constructor
	HippocampusPathControl(char *type_ctrl);

	// Destructor, also kills the main task
	~HippocampusPathControl();

	// Start the multicopter attitude control task, @return OK on success.
	int	start();

private:

	bool	_task_should_exit;		// if true, task_main() should exit
	int		_control_task;			// task handle

	// topic subscriptions

	int		_v_att_sub;		        // orientation data
	int     _v_pos_sub;             // position data
	int		_params_sub;			// parameter updates subscription
	int		_v_traj_sp_sub;			// trajectory setpoint subscription
    int		_debug_value_sub;		 // debug_value
    int		_debug_vect_sub;		 // debug_value
    int     _actuator_outputs_sub;
	int     sd_save;
	int     yaw_drift_compensation_init;
  //  int		_sensors_sub;

	// topic publications
	orb_advert_t	_actuators_0_pub;		    // attitude actuator controls publication
	orb_advert_t    _logging_hippocampus_pub;   // logging data publisher
	orb_id_t        _actuators_id;	            // pointer to correct actuator controls0 uORB metadata structure
	orb_advert_t	_debug_value_pub;
	orb_advert_t	_debug_vect_pub;		    // attitude actuator controls publication
    //orb_advert_t        _sensors_sub;

	// topic structures, in this structures the data of the topics are stored
	struct actuator_controls_s			_actuators;			    // actuator controls
	struct logging_hippocampus_s        _logging_hippocampus;   // logging data
	struct vehicle_attitude_s		    _v_att;		            // attitude data
	struct vehicle_local_position_s		_v_pos;		            // attitude data
	struct trajectory_setpoint_s	    _v_traj_sp;			    // trajectory setpoint
	struct debug_vect_s	            _debug_vect;
	struct debug_value_s	            _debug_value;			    // debug value
	struct sensor_combined_s	        sensors;			    // sensors
	struct actuator_outputs_s            _actuator_output;
	struct pressure_s                   _press;
	// performance counters
	perf_counter_t	_loop_perf;
	perf_counter_t	_controller_latency_perf;

	// storage vectors for position, velocity, acceleration and the derivative of the acceleration
	math::Vector<3>     _position_0;        // actual position
	math::Vector<3>     _position_1;        // previous position
	math::Vector<3>     _position_2;
	math::Vector<3>     _position_3;
    math::Vector<3>    _omega;
	// time
	float               t_ges;
	float               counter;
	float               yaw_counter;
	float               yaw_compensated;
    float               yaw_drift_compensation;
    float               pi;

    	    /* Parameters for pressure sensor */
    int         _pressure_raw;          /**< pressure sensor subscription */
    float       _depth;                 /**< measured depth in m */
    int         _counter;               /**< Counter for saving _p_zero */
    float       _p_zero;
    float       _roh_g;
    float       _pressure_new;
    float       _pressure_time_old;
    float       _pressure_time_new;
    float       _iterationtime;    /**< Time per Iteration */
        /**< SLIDING-MODE-OBSERVER (SMO) */
        float _xhat1;            /**< Estimated depth in m */
        float _xhat2;            /**< Estimated velocity in m/s */
        float _xhat1_prev;       /**< Estimated depth at previous time step in m */
        float _xhat2_prev;       /**< Estimated velocity at previous time step in m/s */
        float _depth_smo;  /**< Outcome water depth SMO in m */
    float       _depth_err;              /**< Error actual water depth and desired water depth */
    float       _depth_p;
    float       _depth_i;
    float       _depth_i_old;
    float       _depth_d;
    float       _depth_pid;

    /* Parameters for Attitude Controller */
        float       _roll_angle;
        float       _roll_err;
        float       _roll_p;
        float       _roll_d;
        float       _roll_pd;

        float       _pitch_angle;
        float       _pitch_err;
        float       _pitch_p;
        float       _pitch_d;
        float       _yaw_pd;
        float       _yaw_p;
        float       _yaw_d;

    // controller type
	char type_array[100];

	math::Matrix<3, 3>  _I;				// identity matrix

	struct {
		param_t K_PX;
		param_t K_PY;
		param_t K_PZ;
		param_t K_VX;
		param_t K_VY;
		param_t K_VZ;
		param_t K_RX;
		param_t K_RY;
		param_t K_RZ;
		param_t K_WX;
		param_t K_WY;
		param_t K_WZ;
		param_t m;
		param_t X_du;
		param_t Y_dv;
		param_t Z_dw;
		param_t X_u;
		param_t Y_v;
		param_t Z_w;
		param_t K_F;
		param_t K_M;
		param_t L;
		param_t OG_THRUST;
		param_t OG_YAW;
		param_t OG_PITCH;
		param_t OG_ROLL;
		param_t PI_RO_ONLY;
		param_t ROLL;
		param_t PITCH;
		param_t YAW;
		param_t SCALE;
		param_t MIX;
		param_t WS_CONTROL;
		param_t NO_BACK;

		 param_t depth_sp;
         param_t depth_p_gain;
         param_t depth_i_gain;
         param_t depth_d_gain;

         // SMO: Model parameters
         param_t rho; // Observer parameter
         param_t tau; // Observer parameter
         param_t phi; // Observer parameter

         //Attitude Controller
         param_t roll_gain;
         param_t pitch_gain;
         param_t yaw_gain;
         param_t roll_rate_gain;
         param_t pitch_rate_gain;
         param_t yaw_rate_gain;
         param_t thrust_sp;
         param_t yaw_rate_sp;

         param_t roll_sp;
         param_t pitch_sp;

         param_t weight_pitch;
         param_t weight_depth;
	}		_params_handles;		// handles for to find parameters

	struct {
		// gain matrices
		math::Matrix<3, 3> K_p;         // position
		math::Matrix<3, 3> K_v;         // velocity
		math::Matrix<3, 3> K_r;         // orientation
		math::Matrix<3, 3> K_w;         // angular velocity
		// Hippocampus object parameters
		float m;                        // mass
		math::Matrix<3, 3> M_A;         // added mass matrix
		math::Matrix<3, 3> D;           // Damping Matrix
		// Force and Moment scaling factors
		float k_F;
		float k_M;
		// Lifting arm
		float L;
		float OG_thrust;                       // operating grade
		float OG_yaw;                       // operating grade
		float OG_pitch;                       // operating grade
		float OG_roll;                       // operating grade
		float roll;
		float pitch;
		float yaw;
		int scale;
		int mix;
		int pi_ro_only;                       // set pitch and roll only
		int WS_control;                       // set pitch and roll only
        int no_back;
		 float depth_sp;      /**> Desired depth */
         float depth_p_gain;
         float depth_i_gain;
         float depth_d_gain;

         //SLIDING-MODE-OBSERVER (SMO)
         float rho;
         float tau;
         float phi;

                  //Attitude Controller
         float roll_gain;
         float pitch_gain;
         float yaw_gain;
         float roll_rate_gain;
         float pitch_rate_gain;
         float yaw_rate_gain;
         float thrust_sp;
         float yaw_rate_sp;

         float roll_sp;
         float pitch_sp;

         float weight_pitch;
         float weight_depth;
	}		_params;

	// actualizes position data
	void        actualize_position();

	// writes position data into variables
	void        get_position(math::Vector<3> &x, math::Vector<3> &dx, math::Vector<3> &ddx, math::Vector<3> &dddx,
				 double dt);

	// update actual trajectory setpoint
	void        trajectory_setpoint_poll();

	// get orientation error
	math::Vector<3> rotError(math::Matrix<3,3> R, math::Matrix<3,3> R_des);

	// Get Flow Velocity
	math::Vector<3> flowField(math::Vector<3> pos);

	// path controller.
	void		path_control(float dt);

	// Update our local parameter cache.
	int			parameters_update();                // checks if parameters have changed and updates them

	// Check for parameter update and handle it.
	void		parameter_update_poll();            // receives parameters

	// Shim for calling task_main from task_create.
	static void	task_main_trampoline(int argc, char *argv[]);

	// Main attitude control task.
	void		task_main();

	float get_xhat2(float x1, float iterationtime);

    float get_xhat1(float x1, float iterationtime);

    // sat functio
    float sat(float x, float gamma);
};


namespace path_contr
{
HippocampusPathControl	*g_control;
}

// constructor of class HippocampusPathControl
HippocampusPathControl::HippocampusPathControl(char *type_ctrl) :

	// First part is about function which are called with the constructor
	_task_should_exit(false),
	_control_task(-1),

	// subscriptions
	_v_att_sub(-1),
	_v_pos_sub(-1),
	_params_sub(-1),
	_v_traj_sp_sub(-1),
	_debug_value_sub(-1),
	_debug_vect_sub(-1),
	_actuator_outputs_sub(-1),
	//_sensors_sub(-1),

	// publications
	_actuators_0_pub(nullptr),
	_logging_hippocampus_pub(nullptr),
	_actuators_id(nullptr),
	_debug_value_pub(nullptr),
	_debug_vect_pub(nullptr),


	// performance counters
    _loop_perf(perf_alloc(PC_ELAPSED, "path_contr")),
	_controller_latency_perf(perf_alloc_once(PC_ELAPSED, "ctrl_latency"))

// here starts the allocation of values to the variables
{
	// define publication settings
	memset(&_v_att, 0, sizeof(_v_att));
	memset(&_v_pos, 0, sizeof(_v_pos));
	memset(&_actuators, 0, sizeof(_actuators));
	memset(&_logging_hippocampus, 0, sizeof(_logging_hippocampus));
	memset(&_v_traj_sp, 0, sizeof(_v_traj_sp));


    // set parameters to zero
	_params.K_p.zero();
	_params.K_v.zero();
	_params.K_r.zero();
	_params.K_w.zero();
	_params.m = 0.0f;
	_params.M_A.zero();
	_params.D.zero();
	_params.L = 0.0f;
	_params.OG_thrust = 0.0f;
	_params.OG_yaw = 0.0f;
	_params.OG_pitch = 0.0f;
	_params.OG_roll = 0.0f;
	_params.pi_ro_only = 0;
	_params.roll = 0.0f;
	_params.pitch = 0.0f;
	_params.yaw = 0.0f;
	_params.WS_control = 0.0f;
	_params.no_back = 0.0f;
	sd_save = 0;

	// set initial values of vectors to zero
	_position_0.zero();
	_position_1.zero();
	_position_2.zero();
	_position_3.zero();

	t_ges = 0.0;
	counter = 0.0;
	yaw_compensated = 0.0f;
    yaw_drift_compensation = 0.0f;
    pi = M_PI;
    yaw_drift_compensation_init = 0;

    	/* Pressure Sensor */
    _depth = 0;
    _depth_i_old = 0;
    _roh_g = 98.1;
    _counter = 1;
    _p_zero = 0.0;
    _pressure_new = 0;
    _pressure_time_old = 0;
    _pressure_time_new = 0;
    _iterationtime = 0;
    _xhat1 = 0.0; //Estimated depth in m
    _xhat2 = 0.0; //Estimated velocity in m/s
    _xhat1_prev = 0; //Estimated depth at previous time step in m
    _xhat2_prev = 0; //Estimated velocity at previous time step in m/s

	// allocate controller type
	strcpy(&type_array[0], type_ctrl);

	if (!strcmp(type_array, "full")) {
		PX4_INFO("Start full geometric controller!");

	} else if (!strcmp(type_array, "attitude")) {
		PX4_INFO("Start attitude controller!");

	} else if (!strcmp(type_array, "WS")) {
		PX4_INFO("Start WORK-SHOP controller!");
	}

	// allocate Identity matrix
	_I.identity();

	// allocate parameter handles
	_params_handles.K_PX	        = 	param_find("PC_K_PX");
	_params_handles.K_PY	        = 	param_find("PC_K_PY");
	_params_handles.K_PZ	        = 	param_find("PC_K_PZ");
	_params_handles.K_VX		    = 	param_find("PC_K_VX");
	_params_handles.K_VY		    = 	param_find("PC_K_VY");
	_params_handles.K_VZ		    = 	param_find("PC_K_VZ");
	_params_handles.K_RX		    = 	param_find("PC_K_RX");
	_params_handles.K_RY		    = 	param_find("PC_K_RY");
	_params_handles.K_RZ		    = 	param_find("PC_K_RZ");
	_params_handles.K_WX		    = 	param_find("PC_K_WX");
	_params_handles.K_WY		    = 	param_find("PC_K_WY");
	_params_handles.K_WZ		    = 	param_find("PC_K_WZ");
	_params_handles.m		        = 	param_find("PC_m");
    _params_handles.X_du		    = 	param_find("PC_X_du");
	_params_handles.Y_dv		    = 	param_find("PC_Y_dv");
	_params_handles.Z_dw		    = 	param_find("PC_Z_dw");
	_params_handles.X_u		        = 	param_find("PC_X_u");
	_params_handles.Y_v		        = 	param_find("PC_Y_v");
	_params_handles.Z_w		        = 	param_find("PC_Z_w");
	_params_handles.K_F		        = 	param_find("PC_K_F");
	_params_handles.K_M		        = 	param_find("PC_K_M");
	_params_handles.L	            = 	param_find("PC_L");
	_params_handles.OG_THRUST	    = 	param_find("PC_OG_THRUST");
    _params_handles.OG_YAW	        = 	param_find("PC_OG_YAW");
    _params_handles.OG_PITCH	    = 	param_find("PC_OG_PITCH");
    _params_handles.OG_ROLL	        = 	param_find("PC_OG_ROLL");
	_params_handles.ROLL	        = 	param_find("PC_ROLL");
	_params_handles.PITCH	        = 	param_find("PC_PITCH");
	_params_handles.YAW	            = 	param_find("PC_YAW");
    _params_handles.SCALE	        = 	param_find("PC_SCALE");
    _params_handles.MIX	            = 	param_find("PC_MIX");
    _params_handles.PI_RO_ONLY	    = 	param_find("PC_PI_RO_ONLY");
    _params_handles.WS_CONTROL	    = 	param_find("PC_WS_CONTROL");
    _params_handles.NO_BACK	    = 	param_find("PC_NO_BACK");



    _params_handles.depth_sp        =   param_find("PC_DEPTH");
    _params_handles.depth_p_gain    =   param_find("PC_DEPTH_P");
    _params_handles.depth_i_gain    =   param_find("PC_DEPTH_I");
    _params_handles.depth_d_gain    =   param_find("PC_DEPTH_D");
    _params_handles.rho             =   param_find("PC_RHO");
    _params_handles.tau             =   param_find("PC_TAU");
    _params_handles.phi             =   param_find("PC_PHI");

       _params_handles.roll_gain       =   param_find("PC_ROLL_GAIN");
    _params_handles.pitch_gain      =   param_find("PC_PITCH_GAIN");
    _params_handles.yaw_gain        =   param_find("PC_YAW_GAIN");
    _params_handles.roll_rate_gain  =   param_find("PC_ROLL_RATE_G");
    _params_handles.yaw_rate_gain  =   param_find("PC_YAW_RATE_G");
    _params_handles.pitch_rate_gain =   param_find("PC_PITCH_RATE_G");
    _params_handles.thrust_sp       =   param_find("PC_THRUST_SP");
    _params_handles.yaw_rate_sp     =   param_find("PC_YAW_RATE_SP");
    _params_handles.roll_sp         =   param_find("PC_ROLL_SP_DEG");
    _params_handles.pitch_sp         =   param_find("PC_PITCH_SP_DEG");

    _params_handles.weight_pitch    =   param_find("PC_WEIGHT_PITCH");
    _params_handles.weight_depth    =   param_find("PC_WEIGHT_DEPTH");
	// fetch initial parameter values
	parameters_update();
}

// destructor of class HippocampusPathControl
HippocampusPathControl::~HippocampusPathControl()
{
	if (_control_task != -1) {
		/* task wakes up every 100ms or so at the longest */
		_task_should_exit = true;

		/* wait for a second for the task to quit at our request */
		unsigned i = 0;

		do {
			/* wait 20ms */
			usleep(20000);

			/* if we have given up, kill it */
			if (++i > 50) {
				px4_task_delete(_control_task);
				break;
			}
		} while (_control_task != -1);
	}

        path_contr::g_control = nullptr;
}

// updates parameters
int HippocampusPathControl::parameters_update()
{
	float v;

	param_get(_params_handles.K_PX, &v);
	_params.K_p(0,0) = v;
	param_get(_params_handles.K_PY, &v);
	_params.K_p(1,1) = v;
	param_get(_params_handles.K_PZ, &v);
	_params.K_p(2,2) = v;
	param_get(_params_handles.K_VX, &v);
	_params.K_v(0,0) = v;
	param_get(_params_handles.K_VY, &v);
	_params.K_v(1,1) = v;
	param_get(_params_handles.K_VZ, &v);
	_params.K_v(2,2) = v;
	param_get(_params_handles.K_RX, &v);
	_params.K_r(0,0) = v;
	param_get(_params_handles.K_RY, &v);
	_params.K_r(1,1) = v;
	param_get(_params_handles.K_RZ, &v);
	_params.K_r(2,2) = v;
	param_get(_params_handles.K_WX, &v);
	_params.K_w(0,0) = v;
	param_get(_params_handles.K_WY, &v);
	_params.K_w(1,1) = v;
	param_get(_params_handles.K_WZ, &v);
	_params.K_w(2,2) = v;
	param_get(_params_handles.m, &v);
	_params.m = v;
	param_get(_params_handles.X_du, &v);
	_params.M_A(0, 0) = v;
	param_get(_params_handles.Y_dv, &v);
	_params.M_A(1, 1) = v;
	param_get(_params_handles.Z_dw, &v);
	_params.M_A(2, 2) = v;
	param_get(_params_handles.X_u, &v);
	_params.D(0, 0) = v;
	param_get(_params_handles.Y_v, &v);
	_params.D(1, 1) = v;
	param_get(_params_handles.Z_w, &v);
	_params.D(2, 2) = v;
	param_get(_params_handles.K_F, &v);
	_params.k_F = v;
	param_get(_params_handles.K_M, &v);
	_params.k_M = v;
	param_get(_params_handles.L, &v);
	_params.L = v;
    param_get(_params_handles.OG_THRUST, &v);
	_params.OG_thrust = v;
	param_get(_params_handles.OG_YAW, &v);
	_params.OG_yaw = v;
	param_get(_params_handles.OG_PITCH, &v);
	_params.OG_pitch = v;
	param_get(_params_handles.OG_ROLL, &v);
	_params.OG_roll = v;
	param_get(_params_handles.PI_RO_ONLY, &v);
	_params.pi_ro_only = v;
	param_get(_params_handles.ROLL, &v);
	_params.roll = v;
	param_get(_params_handles.PITCH, &v);
	_params.pitch = v;
	param_get(_params_handles.YAW, &v);
	_params.yaw = v;
	param_get(_params_handles.SCALE, &v);
	_params.scale = v;
	param_get(_params_handles.MIX, &v);
	_params.mix = v;
	param_get(_params_handles.WS_CONTROL, &v);
	_params.WS_control = v;
	param_get(_params_handles.NO_BACK, &v);
	_params.no_back = v;

	   param_get(_params_handles.depth_sp, &(_params.depth_sp));
    param_get(_params_handles.depth_p_gain, &(_params.depth_p_gain));
    param_get(_params_handles.depth_i_gain, &(_params.depth_i_gain));
    param_get(_params_handles.depth_d_gain, &(_params.depth_d_gain));
    param_get(_params_handles.rho, &(_params.rho));
    param_get(_params_handles.tau, &(_params.tau));
    param_get(_params_handles.phi, &(_params.phi));
    param_get(_params_handles.roll_gain, &(_params.roll_gain));
    param_get(_params_handles.yaw_gain, &(_params.yaw_gain));
    param_get(_params_handles.pitch_gain, &(_params.pitch_gain));
    param_get(_params_handles.yaw_gain, &(_params.yaw_gain));
    param_get(_params_handles.roll_rate_gain, &(_params.roll_rate_gain));
    param_get(_params_handles.yaw_rate_gain, &(_params.yaw_rate_gain));
    param_get(_params_handles.pitch_rate_gain, &(_params.pitch_rate_gain));
    param_get(_params_handles.yaw_rate_gain, &(_params.yaw_rate_gain));
    param_get(_params_handles.thrust_sp, &(_params.thrust_sp));
    param_get(_params_handles.yaw_rate_sp, &(_params.yaw_rate_sp));
    param_get(_params_handles.roll_sp, &(_params.roll_sp));
    param_get(_params_handles.pitch_sp, &(_params.pitch_sp));
    param_get(_params_handles.weight_pitch, &(_params.weight_pitch));
    param_get(_params_handles.weight_depth, &(_params.weight_depth));

	return OK;
}

// check for parameter updates
void HippocampusPathControl::parameter_update_poll()
{
	bool updated;

	/* Check if parameters have changed */
	orb_check(_params_sub, &updated);

	if (updated) {
		struct parameter_update_s param_update;
		orb_copy(ORB_ID(parameter_update), _params_sub, &param_update);
		parameters_update();

		PX4_INFO("K_p:\t%8.2f\t%8.2f\t%8.2f",
			 (double)_params.K_p(0,0),
			 (double)_params.K_p(1,1),
			 (double)_params.K_p(2,2));
		PX4_INFO("K_v:\t%8.2f\t%8.2f\t%8.2f",
			 (double)_params.K_v(0,0),
			 (double)_params.K_v(1,1),
			 (double)_params.K_v(2,2));
		PX4_INFO("K_r:\t%8.2f\t%8.2f\t%8.2f",
			 (double)_params.K_r(0,0),
			 (double)_params.K_r(1,1),
			 (double)_params.K_r(2,2));
		PX4_INFO("K_w:\t%8.2f\t%8.2f\t%8.2f\n",
			 (double)_params.K_w(0,0),
			 (double)_params.K_w(1,1),
			 (double)_params.K_w(2,2));

	}
}


// actualizes the position data if receiving new data
void HippocampusPathControl::actualize_position()
{

	// write position data into vectors
	math::Vector<3> holder(_v_pos.x, _v_pos.y, _v_pos.z);

	// store position data
	_position_3 = _position_2;
	_position_2 = _position_1;
	_position_1 = _position_0;
	_position_0 = holder;
}


//Calculates the velocity and accelerations based on the position using differential quotient
void HippocampusPathControl::get_position(math::Vector<3> &x, math::Vector<3> &dx, math::Vector<3> &ddx,
		math::Vector<3> &dddx, double dt)
{
	x = _position_0;
	dx = (_position_0 - _position_1) / dt;
	ddx = (_position_0 - _position_1 * 2.0 + _position_2) / (dt * dt);
	dddx = (_position_0 - _position_1 * 2.0 + _position_2 * 2.0 - _position_3) / (2.0 * dt * dt * dt);
}

// Get the Setpoint from the trajectory planner
void HippocampusPathControl::trajectory_setpoint_poll()
{
	/* check if there is a new setpoint */
	bool updated;
	orb_check(_v_traj_sp_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(trajectory_setpoint), _v_traj_sp_sub, &_v_traj_sp);
	}
}

float HippocampusPathControl::get_xhat2(float x1, float time) {

    _xhat1 = get_xhat1(x1, time);

   // xhat2 = xhat2_prev + (iterationtime / (0.1 * tau))* (-xhat2_prev - rho * sat(xhat1 - x1,1));
    _xhat2 = _xhat2_prev + (time / _params.tau) * (-_xhat2_prev - _params.rho * sat(_xhat1 - x1,_params.phi));

    _xhat2_prev = _xhat2;
    return _xhat2; //xhat2 = geschätzte Geschwindigkeit
}


float HippocampusPathControl::get_xhat1(float x1, float time) {
    //xhat1 = xhat1_prev - (iterationtime / 0.1) * rho * sat(xhat1_prev - x1, 1);
    _xhat1 = _xhat1_prev - (time / 1) * _params.rho * sat(_xhat1_prev - x1, _params.phi);
    _xhat1_prev = _xhat1; //xhat1 = geschätzte Tiefe
    return _xhat1;
}

// sat functio
float HippocampusPathControl::sat(float x, float gamma) {
    float y = math::max(math::min(1.0f, x / gamma), -1.0f);
    return y;
}



// Gives back orientation error between R and R_des
math::Vector<3> HippocampusPathControl::rotError(math::Matrix<3,3> R, math::Matrix<3,3> R_des)
{
    // extract orientation vectors
    math::Vector<3> x_B = R.get_colValues(0);
    math::Vector<3> x_B_des = R_des.get_colValues(0);
    math::Vector<3> z_B_des = R_des.get_colValues(2);

    // extracting one rotation from the rotation matrix, necessary due to singularities in the (R_des^T * R - R^T * R_des) approach
	// rotation axis for rotation between x_B and x_B_des, in B coordinates (not normalized yet)
	math::Vector<3> e_r = R.transposed() * (x_B_des % x_B);

	// calculate the angle errors using norm of cross product and dot product
	float x_B_sin = e_r.length();
	float x_B_cos = x_B * x_B_des;

	// rotation matrix after pitch/yaw only rotation, thus between R and R_py are only a roll rotation left
	math::Matrix<3, 3> R_py;

	// check if x_B and x_B_des are non parallel, otherwise we would not have rotations pitch and yaw
	if (x_B_sin > 0.0f) {
		// calculate axis angle representation of the pitch/yaw rotation
		float e_R_angle = atan2f(x_B_sin, x_B_cos);
		math::Vector<3> e_R_axis = e_r / x_B_sin;           // normalize axis

		// get the error vector of the rotation in B coordinates
		e_r = e_R_axis * e_R_angle;

		// get the cross product matrix for e_R_axis to calculate the Rodrigues formula
		math::Matrix<3, 3> e_R_cp;
		e_R_cp.zero();
		e_R_cp(0, 1) = -e_R_axis(2);
		e_R_cp(0, 2) = e_R_axis(1);
		e_R_cp(1, 0) = e_R_axis(2);
		e_R_cp(1, 2) = -e_R_axis(0);
		e_R_cp(2, 0) = -e_R_axis(1);
		e_R_cp(2, 1) = e_R_axis(0);

		// rotation matrix after pitch/yaw only rotation, thus between R and R_py are only a roll rotation left, in World coordinates
		R_py = R * (_I + e_R_cp * x_B_sin + e_R_cp * e_R_cp * (1.0f - x_B_cos));

	} else {
		// zero pitch/yaw rotation
		R_py = R;
	}

	//R_py and R_des have the same X axis, calculate roll error
	math::Vector<3> z_B_py(R_py(0, 2), R_py(1, 2), R_py(2, 2));
	e_r(0) = atan2f((z_B_des % z_B_py) * x_B_des, z_B_py * z_B_des);

	return e_r;
}

math::Vector<3> HippocampusPathControl::flowField(math::Vector<3> pos)
{
  // Get Current Velocity, only irrotational velocities considered
  math::Vector<3> flowVel(0.0f, 0.0f, 0.0f);            // Initialize Flow Velocity
  if (pos(1) > 2.0f && pos(1) < 6.0f) {                 // Check if Field is in current
    flowVel(0) = 0.0f;
  }
  return flowVel;
}

/**
 * path controller.
 * Input: 'setpoints from trajectory planner', 'actual position and orientation'
 * Output: 'actuators_control'
 */
void HippocampusPathControl::path_control(float dt)
{



    // actualize setpoint data
	trajectory_setpoint_poll();
	// count time
	t_ges = t_ges + dt;

	//*******************************
	//  Declaration of Variables
	//*******************************
	// define error vectors
	math::Vector<3> e_p;            // position error
	math::Vector<3> e_v;            // velocity error
	math::Vector<3> e_r;            // orientation error
	math::Matrix<3, 3> e_r_matrix;  // orientation error matrix
	math::Vector<3> e_w;            // angular velocity error

	// get actual position data
	math::Vector<3> r;                      // actual position
 	math::Vector<3> rd;                     // actual velocity
	math::Vector<3> rdd;                    // actual acceleration
	math::Vector<3> rddd;
	actualize_position();                   // record new position data
	get_position(r, rd, rdd, rddd, dt);     // estimate derivations using difference methods


	// get trajectory setpoint data
	math::Vector<3> r_T(_v_traj_sp.x, _v_traj_sp.y, _v_traj_sp.z);                  // desired position
	math::Vector<3> rd_T(_v_traj_sp.dx, _v_traj_sp.dy, _v_traj_sp.dz);              // desired velocity
	math::Vector<3> rdd_T(_v_traj_sp.ddx, _v_traj_sp.ddy, _v_traj_sp.ddz);          // desired acceleration
	math::Vector<3> rddd_T(_v_traj_sp.dddx, _v_traj_sp.dddy, _v_traj_sp.dddz);

	// desired force and outputs
	math::Vector<3> F_des;
    float u_1 = 0.0f;
	math::Vector<3> u_24;

	// rotation matrices and angular velocity vectors
	math::Vector<3> w_BW(_v_att.rollspeed, _v_att.pitchspeed, _v_att.yawspeed);      // angular velocity
	math::Vector<3> w_BW_T;                                                                         // desired angular velocity
	math::Matrix<3, 3> R;                                                                           // actual rotation matrix
	math::Matrix<3, 3> R_des;                                                                       // desired rotation matrix

	// get current rotation matrix from control state quaternions, the quaternions are generated by the
	// attitude_estimator_q application using the sensor data
	math::Quaternion q_att(_v_att.q[0], _v_att.q[1], _v_att.q[2], _v_att.q[3]);
	// create rotation matrix for the quaternion when post multiplying with a column vector x'=R*x
	R = q_att.to_dcm();

	// transform angular velocity into World coordinate system since the given control state data are in body frame
	w_BW = R * w_BW;

	// orientation vectors
	math::Vector<3> x_B(R(0, 0), R(1, 0), R(2,
						0));                             // orientation body x-axis (in world coordinates)
	math::Vector<3> y_B(R(0, 1), R(1, 1), R(2,
						1));                             // orientation body y-axis (in world coordinates)
	math::Vector<3> z_B(R(0, 2), R(1, 2), R(2,
						2));                             // orientation body z-axis (in world coordinates)
	math::Vector<3> x_B_des;                                                    // orientation body x-axis desired
	math::Vector<3> y_B_des;                                                    // orientation body y-axis desired
	math::Vector<3> z_B_des;                                                    // orientation body z-axis desired

	// Get Flow Velocity
    math::Vector<3> flowVel = flowField(r);

	// Transform Matrices into World coordinates
    math::Matrix<3,3> M_A_W = R * _params.M_A * R.transposed();
    math::Matrix<3,3> D_W = R * _params.D * R.transposed();

    //euler angles
    matrix::Eulerf euler = matrix::Quatf(_v_att.q);


	if (!strcmp(type_array, "full")) {
        math::Vector<3> z_C_des(0, -sinf(_v_traj_sp.roll), cosf(_v_traj_sp.roll));    // orientation C-Coordinate system desired
	    math::Vector<3> y_C_des(0, cosf(_v_traj_sp.roll), sinf(_v_traj_sp.roll));     // orientation C-Coordinate system desired

	    // projection on x_B
	    math::Vector<3> h_w;
	    math::Vector<3> h_w_des;

	    // thrust input
	    e_p = r - r_T;                      // calculate position error
	    e_v = rd - rd_T;                    // calculate velocity error

        // calculate desired force
        math::Vector<3> rd_e = rd_T - flowVel;
	    F_des = - _params.K_p * e_p - _params.K_v * e_v + rdd_T * _params.m + M_A_W * rdd_T + D_W * rd_e;
        F_des(2) = 0;                                       // for only movement in x-y plane
	    u_1 = F_des * x_B;                                 // calculate desired thrust

	    // calculate orientation vectors
	    x_B_des = F_des / F_des.length();

	    // check wether a rotation matrix created using y_B_des is closer to the actual position or z_B_des
	    math::Vector<3> y_B_des_1 = z_C_des % x_B_des;
	    y_B_des_1 = y_B_des_1 / y_B_des_1.length();
	    math::Vector<3> z_B_des_1 = x_B_des % y_B_des_1;

	    math::Vector<3> z_B_des_2 = x_B_des % y_C_des;
	    z_B_des_2 = z_B_des_2 / z_B_des_2.length();
	    math::Vector<3> y_B_des_2 = z_B_des_2 % x_B_des;

	    // calculate desired rotation matrix and error
	    math::Matrix<3,3> R_des_1;
	    R_des_1.set_col(0, x_B_des);
	    R_des_1.set_col(1, y_B_des_1);
	    R_des_1.set_col(2, z_B_des_1);
	    math::Vector<3> e_r_1 = rotError(R, R_des_1);

	    math::Matrix<3,3> R_des_2;
	    R_des_2.set_col(0, x_B_des);
	    R_des_2.set_col(1, y_B_des_2);
	    R_des_2.set_col(2, z_B_des_2);
	    math::Vector<3> e_r_2 = rotError(R, R_des_2);

	    // check which orientation error is smaller and use this as desired orientation
	    if (e_r_1.length() < e_r_2.length()) {
	        e_r = e_r_1;
	        R_des = R_des_1;
	        y_B_des = y_B_des_1;
	        z_B_des = z_B_des_1;
	    } else {
	        e_r = e_r_2;
	        R_des = R_des_2;
	        y_B_des = y_B_des_2;
	        z_B_des = z_B_des_2;
	    }

        float d = 10;
        float m_a = 1.5;

	    // Calculate desired angular velocity
	    h_w_des = (rddd_T * _params.m + rddd_T * m_a + rdd_T * d - (x_B_des * ((rddd_T * _params.m + rddd_T * m_a
	                + rdd_T * d) * x_B_des))) * (1 / u_1);

	    double q_ang = -h_w_des * z_B_des;
	    double r_ang = -h_w_des * y_B_des;
	    double p_ang = _v_traj_sp.droll * x_B_des(0);
	    w_BW_T = x_B_des * p_ang + y_B_des * q_ang + z_B_des * r_ang;

	    // calculate the angular velocity error
	    e_w = R.transposed() * w_BW - R.transposed() * w_BW_T;



	} else if (!strcmp(type_array, "attitude")) {
	    float c_roll = cosf(_params.roll);
	    float s_roll = sinf(_params.roll);
	    float c_pitch = cosf(_params.pitch);
	    float s_pitch = sinf(_params.pitch);
	    float c_yaw = cosf(_params.yaw);
	    float s_yaw = sinf(_params.yaw);

	    R_des(0,0) = c_pitch*c_yaw;
	    R_des(0,1) = s_roll*s_pitch*c_yaw - c_roll*s_yaw;
	    R_des(0,2) = c_roll*s_pitch*c_yaw + s_roll*s_yaw;
	    R_des(1,0) = c_pitch*s_yaw;
	    R_des(1,1) = s_roll*s_pitch*s_yaw + c_roll*c_yaw;
	    R_des(1,2) = c_roll*s_pitch*s_yaw - s_roll*c_yaw;
	    R_des(2,0) = -s_pitch;
	    R_des(2,1) = s_roll*c_pitch;
	    R_des(2,2) = c_roll*c_pitch;

        e_r = rotError(R, R_des);

	    x_B_des = R_des.get_colValues(0);
	    y_B_des = R_des.get_colValues(1);
	    z_B_des = R_des.get_colValues(2);

        u_1 = 0.0f;
        e_w = R.transposed() * w_BW;

	}else if (!strcmp(type_array, "WS")) {

	    //YAW_Control
        float e_r_1;
        //float e_r_2;
	    float psi_des = atan2f(r_T(1)-r(1),r_T(0)-r(0));
	    //e_r(2) = psi_des-euler.psi();

        e_r_1 = psi_des-euler.psi();
        /*e_r_2 = euler.psi()-psi_des;

          if (abs(e_r_1) > abs(e_r_2)){
	    e_r(2) = e_r_2;
	    }   else{
	    e_r(2) = e_r_1;
	    }
        */
        if(e_r_1>3.1416f){
            e_r(2) = e_r_1-3.1416f*2.0f;
            }else if(e_r_1<-3.1416f){
            e_r(2)=e_r_1+3.1416f*2.0f;
            }else{
            e_r(2) = e_r_1;
            }
	   // if (e_r(2) > 4.0f){
	   // e_r(2) = e_r(2) - 2.0f * pi;
	   // }   else if (e_r(2) < -4.0f){
	   // e_r(2) = e_r(2) + 2.0f* pi;
	   // }

	    float yaw_des = 0.0f;
        e_w (2) = _v_att.yawspeed - yaw_des ;


            /* Calculate P-term of PD-Controller (Roll) */
            _yaw_p = e_r(2) * _params.yaw_gain;

            /* Calculate D-term of PD-Controller (Roll) */
            _yaw_d = e_w (2) * _params.yaw_rate_gain;

            /* Calculate PD-Controller */
            _yaw_pd = _yaw_p + _yaw_d;


        //TOBIS PITCH CONTROL
   //matrix::Eulerf euler = matrix::Quatf(_v_att.q);

        /* get current rates from sensors */
            _omega(0) = _v_att.rollspeed;
            _omega(1) = _v_att.pitchspeed;
            _omega(2) = _v_att.yawspeed;

        /** get pressure value from sensor*/
            orb_copy(ORB_ID(pressure), _pressure_raw, &_press);
            //orb_copy(ORB_ID(actuator_outputs), _actuator_output, &_act_out);

            /* set surface air pressure  */
            if (_counter == 1){
                _p_zero = 1;
                _p_zero = _press.pressure_mbar;
                _counter = 0;
            }

            /* set actual pressure from the sensor and absolute time to a new controler value */
            _pressure_new = _press.pressure_mbar;
            //_pressure_new = 1;
            _pressure_time_new = hrt_absolute_time();

            /* calculate actual water depth */
            _depth = ( _pressure_new - _p_zero ) / ( _roh_g ); //unit meter


        /**< PID-Controller for Pitch */

            /* Calculate measured pitch angle in degree */
            _pitch_angle = euler.theta() * (180.0f/3.1416f);

            /* Calculate pitch error */
            _pitch_err = _params.pitch_sp - _pitch_angle;

            /* Calculate P-term of PD-Controller (Pitch) */
            _pitch_p = _pitch_err * _params.pitch_gain;

            /* Calculate D-term of PD-Controller (Pitch) */
            _pitch_d = _omega(1) * _params.pitch_rate_gain;


            /* calculate iterationtime for Sliding Mode Observer */
            _iterationtime = _pressure_time_new - _pressure_time_old;
            /* scale interationtime*/
            _iterationtime = _iterationtime * 0.0000015f;

            /* calculate d-component of the controler by using a Sliding Mode Observer for accounting possible future trends of the error */
            _depth_smo = get_xhat1(_depth,_iterationtime);

            /* set actual pressure from the sensor and absolute time to a "old" controler value */
            _pressure_time_old = _pressure_time_new;

            /* calculate the depth error */
            _depth_err = _depth - _params.depth_sp;

            /* P-term of PID-controller */
            _depth_p = _params.depth_p_gain * _depth_err;

            /* I-term of PID-controller */
            _depth_i = _depth_i_old + (_depth_err * _iterationtime);
            _depth_i_old = _depth_i;

            /* PID-Controller */
            _depth_pid = _params.weight_depth * (_depth_p + _params.depth_i_gain * _depth_i + _params.depth_d_gain * _depth_smo) + _params.weight_pitch * (_pitch_p + _pitch_d);

            _debug_vect.x = _depth_p;
            _debug_vect.y = _pitch_p;
            _debug_vect.z = _depth_pid;
            strcpy(_debug_vect.name, "DEBUG_VECT");

            _debug_value.value =_depth ;
            _debug_value.ind = 1;

        /**< Attitude Controller: Roll */

            /* Calculate measured roll angle in degree */
            _roll_angle = euler.phi() * (180.0f/3.1416f);

            /* Calculate roll error */
            _roll_err = _params.roll_sp - _roll_angle;

            /* Calculate P-term of PD-Controller (Roll) */
            _roll_p = _roll_err * _params.roll_gain;

            /* Calculate D-term of PD-Controller (Roll) */
            _roll_d = _omega(0) * _params.roll_rate_gain;

            /* Calculate PD-Controller */
            _roll_pd = _roll_p + _roll_d;
                //Scale
                //_roll_pd = _roll_pd * 0.01f;

        //TRHUST CONTROL
        e_p = r - r_T;                      // calculate position error
	    e_v = rd - rd_T;                    // calculate velocity error
        e_p(2) = _depth_err;
        e_v(2) = _depth_smo;
        // calculate desired force

	    F_des = - _params.K_p * e_p - _params.K_v * e_v + rdd_T * _params.m + M_A_W * rdd_T;
        //F_des(2) = 0;                                       // for only movement in x-y plane

	    u_1 = F_des * x_B;


        if (_params.no_back == 1){
            if (u_1<0.0f){
            u_1 = 0.0f;
            }
        }

	}



//NILS

	// calculate input over feedback loop
	u_24 = -_params.K_r * e_r - _params.K_w * e_w;

	// scale roll with 1/8
	u_24(0) = u_24(0) * (_params.L * _params.k_F / _params.k_M);

	// put all calculated control outputs in one vector
	math::Vector<4> u_ges(u_1, u_24(0), u_24(1), u_24(2));
//    math::Vector<4> u_ges( u_24(0), u_24(1), u_24(2),u_1);


	// Generating K matrix with scaling factors for Forces and Moments
	float K_SCALE[4][4] = {
		{_params.k_F, _params.k_F, _params.k_F, _params.k_F},
		{ -_params.k_M, _params.k_M, -_params.k_M, _params.k_M},
		{ -_params.k_F * _params.L, -_params.k_F * _params.L, _params.k_F * _params.L, _params.k_F * _params.L},
		{_params.k_F * _params.L, -_params.k_F * _params.L, -_params.k_F * _params.L, _params.k_F * _params.L}
	};
	math::Matrix<4, 4> K_scale(K_SCALE);

	math::Vector<4> omega_des = K_scale.inversed() * u_ges;

    //math::Vector<4> omega_des = u_ges;
	// Generating C_mix matrix for recalculation to Mixer Inputs
///*

float C_MIX[4][4];
math::Matrix<4, 4> C_mix(C_MIX);
float C_MIX_sim[4][4]= {
            { -1, -1, 1, 1},
            {1, -1, -1, 1},
            { -1, 1, -1, 1},
            {1, 1, 1, 1}
            };
 float C_MIX_real[4][4]= {
            { -1, -1, 1, 1},
            {-1, 1, 1, -1},
            { -1, 1, -1, 1},
            {-1, -1, -1, -1}
            };
  math::Matrix<4, 4> C_mix_sim(C_MIX_sim);
  math::Matrix<4, 4> C_mix_real(C_MIX_real);

    if (_params.mix == 0){
    //simulation
    C_mix = C_MIX_sim;


    }else if (_params.mix == 1){
    //real environement
    C_mix = C_MIX_real;
    }



	// calculate the desired rotor velocities
	math::Vector<4> mix_input = C_mix.inversed() * omega_des;

	// Reduce Input signal by a certain percentage
	//mix_input = mix_input * _params.OG;

  //  math::Vector<4> mix_input = u_ges * _params.OG;

  //  if (_v_traj_sp.start == 1) {
	// give the inputs to the actuators
    if (_params.scale == 1){

    //Thrust and yaw -control
            _actuators.control[0] = 0 * _params.OG_roll;       // roll
            _actuators.control[1] = 0 * _params.OG_pitch;       // pitch
            _actuators.control[2] = mix_input(2) * _params.OG_yaw; // yaw
            _actuators.control[3] = mix_input(3) * _params.OG_thrust; // thrust


    } else if ( _params.scale == 0){
      //  u_ges = u_ges * _params.OG;
            _actuators.control[0] = 0.0f * _params.OG_roll;// mix_input(0);           // roll
            _actuators.control[1] = 0.0f * _params.OG_pitch;//mix_input(1);           // pitch
            _actuators.control[2] = u_ges(3) * _params.OG_yaw;           // yaw
            _actuators.control[3] = u_ges(0)* _params.OG_thrust;           // thrust

	}
    //End of Nils Controler

    //Actuator Control for uncoupled Controler
	if (_params.WS_control ==1){
	        _actuators.control[0] = _roll_pd * _params.OG_roll;       // roll
            _actuators.control[1] = _depth_pid  * _params.OG_pitch;   // pitch
            _actuators.control[2] =  _yaw_pd * _params.OG_yaw;        // yaw
            _actuators.control[3] = u_1 * _params.OG_thrust;          // thrust

        // Actuautor Control for depth and roll control and const yaw and thrust
            if(_params.pi_ro_only == 1){
              _actuators.control[0] = _roll_pd * _params.OG_roll;       // roll
            _actuators.control[1] = _depth_pid  * _params.OG_pitch;       // pitch
            _actuators.control[2] = _params.OG_yaw; // yaw
            _actuators.control[3] = _params.OG_thrust; // thrust

            }

	}
/*
	    FILE *sd;

	      if (sd_save ==0 ){
            sd = fopen("/fs/microsd/Error_Data.txt","w");
            fprintf(sd,"Position, Position_des, Position_Error and Rotation_Error:\n");
            fclose(sd);
            sd_save =1;
            }

            sd = fopen("/fs/microsd/Error_Data.txt","a");
            fprintf(sd,"\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\n",
            (double)r(0),
			(double)r(1),
            (double)r_T(0),
            (double)r_T(1),
            (double)e_p(0),
			(double)e_p(1),
			(double)e_r(0),
			(double)e_r(1));
            fclose(sd);
*/

//	}

	bool updated;
	orb_check(_actuator_outputs_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(actuator_outputs), _actuator_outputs_sub, &_actuator_output);
	}
     float thrust_p = - _params.K_p * e_p* x_B;
     float thrust_d = - _params.K_v * e_v* x_B;
	// store logging data for publishing
	_logging_hippocampus.x = r(0);
	_logging_hippocampus.y = r(1);
	_logging_hippocampus.z = _depth;
	_logging_hippocampus.xd = r_T(0);
	_logging_hippocampus.yd = r_T(1);
	_logging_hippocampus.zd = _params.depth_sp;
    _logging_hippocampus.e_r[0] = _roll_err;
    _logging_hippocampus.e_r[1] = _pitch_err;
    _logging_hippocampus.e_r[2] = e_r(2);
    _logging_hippocampus.angle[0] = euler.phi();
    _logging_hippocampus.angle[1] = euler.theta();
    _logging_hippocampus.angle[2] = euler.psi();
    _logging_hippocampus.e_p[0] = e_p(0);
    _logging_hippocampus.e_p[1] = e_p(1);
    _logging_hippocampus.e_p[2] = _depth_err;
    _logging_hippocampus.e_v[0] = e_v(0);
    _logging_hippocampus.e_v[1] = e_v(1);
    _logging_hippocampus.e_v[2] = e_v(2);
    _logging_hippocampus.e_w[0] = _omega(0);
    _logging_hippocampus.e_w[1] = _omega(1);
    _logging_hippocampus.e_w[2] = _omega(2);
    _logging_hippocampus.u_in[0] = _actuators.control[0];
    _logging_hippocampus.u_in[1] = _actuators.control[1];
    _logging_hippocampus.u_in[2] = _actuators.control[2];
    _logging_hippocampus.u_in[3] = _actuators.control[3];
    _logging_hippocampus.u_out[0] = _actuator_output.output[0];
    _logging_hippocampus.u_out[1] = _actuator_output.output[1];
    _logging_hippocampus.u_out[2] = _actuator_output.output[2];
    _logging_hippocampus.u_out[3] = _actuator_output.output[3];
    _logging_hippocampus.pitch_out[0] = _pitch_p;
    _logging_hippocampus.pitch_out[1] = _pitch_d;
    _logging_hippocampus.pitch_out[2] = _depth_p;
    _logging_hippocampus.pitch_out[3] = _params.depth_i_gain * _depth_i;
    _logging_hippocampus.pitch_out[4] = _params.depth_d_gain * _depth_smo;
    _logging_hippocampus.pitch_out[5] = _depth_pid ;
    _logging_hippocampus.roll_out[0] = _roll_p;
    _logging_hippocampus.roll_out[1] = _roll_d;
    _logging_hippocampus.yaw_out[0] = _yaw_p;
    _logging_hippocampus.yaw_out[1] = _yaw_d;
    _logging_hippocampus.thrust_out[0] = thrust_p;
    _logging_hippocampus.thrust_out[1] = thrust_d;
	_logging_hippocampus.t = t_ges;



	// Debugging
	if (counter < t_ges) {

		counter = counter + 5.0f;            // every 0.5 seconds

 /*       PX4_INFO("e_p:\t%8.2f\t%8.2f\t%8.2f",
			 (double)e_p(0),
			 (double)e_p(1),
			 (double)e_p(2));
*/
		/*
		PX4_INFO("e_v:\t%8.2f\t%8.2f\t%8.2f",
			 (double)e_v(0),
			 (double)e_v(1),
			 (double)e_v(2));
*/
/*
		PX4_INFO("e_r:\t%8.2f\t%8.2f\t%8.2f",
			 (double)e_r(0),
			 (double)e_r(1),
			 (double)e_r(2));



		PX4_INFO("e_w:\t%8.2f\t%8.2f\t%8.2f\n",
			 (double)e_w(0),
			 (double)e_w(1),
             (double)e_w(2));

        PX4_INFO("r:\t%8.2f\t%8.2f\t%8.2f",
			 (double)r(0),
			 (double)r(1),
             (double)r(2));
		PX4_INFO("r_T:\t%8.2f\t%8.2f\t%8.2f",
			 (double)r_T(0),
			 (double)r_T(1),
			 (double)r_T(2));

	//	PX4_INFO("Thrust:\t%8.2f",
	//		 (double)u_1);
*/
	/*


    PX4_INFO("Actuators_control Origin uges:\t%8.2f\t%8.2f\t%8.2f\t%8.2f",
             (double)u_ges(0),
             (double)u_ges(1),
             (double)u_ges(2),
             (double)u_ges(3));

        PX4_INFO("Actuators_control  scaled:\t%8.2f\t%8.2f\t%8.2f\t%8.2f",
             (double)mix_input(0),
             (double)mix_input(1),
             (double)mix_input(2),
             (double)mix_input(3));
*/
/*
       PX4_INFO("Actuators_control :\t%8.2f\t%8.2f\t%8.2f\t%8.2f",
             (double)_actuators.control[0],
             (double)_actuators.control[1],
             (double)_actuators.control[2],
             (double)_actuators.control[3]);
*/
	}

/*
            if (!strcmp(type_array, "stop")) {
            _actuators.control[0] = 0;           // roll
            _actuators.control[1] = 0;           // pitch
            _actuators.control[2] = 0;           // yaw
            _actuators.control[3] = 0;           // thrust
        }

        */


}

// Just starts the task_main function
void HippocampusPathControl::task_main_trampoline(int argc, char *argv[])
{
        path_contr::g_control->task_main();
}

// this is the main_task function which does the control task
void HippocampusPathControl::task_main()
{

	PX4_INFO("Path Controller has been started!");
	// subscribe to uORB topics
	_v_att_sub = orb_subscribe(ORB_ID(vehicle_attitude));
	_v_pos_sub = orb_subscribe(ORB_ID(vehicle_local_position));
	_params_sub = orb_subscribe(ORB_ID(parameter_update));
	_v_traj_sp_sub = orb_subscribe(ORB_ID(trajectory_setpoint));
	_debug_value_sub = orb_subscribe(ORB_ID(debug_value));
	_debug_vect_sub = orb_subscribe(ORB_ID(debug_vect));
	_actuator_outputs_sub = orb_subscribe(ORB_ID(actuator_outputs));
	int _sensors_sub = orb_subscribe(ORB_ID(sensor_combined));
	_pressure_raw = orb_subscribe(ORB_ID(pressure));

	// initialize parameters cache
	parameters_update();

	// wakeup source: vehicle pose
	px4_pollfd_struct_t fds[1];

	fds[0].fd = _v_att_sub;
	fds[0].events = POLLIN;

	while (!_task_should_exit) {

		// wait for up to 100ms for data, we try to poll the data
		int pret = px4_poll(&fds[0], (sizeof(fds) / sizeof(fds[0])), 100);

		// timed out - periodic check for _task_should_exit
		if (pret == 0) {
			PX4_INFO("Got no data in 100ms!");
			continue;
		}

		// this is undesirable but not much we can do - might want to flag unhappy status
		if (pret < 0) {
                        warn("path_contr: poll error %d, %d", pret, errno);
			// sleep a bit before next try
			usleep(100000);
			continue;
		}

		perf_begin(_loop_perf);

		// run controller on pose changes
		if (fds[0].revents & POLLIN) {
			static uint64_t last_run = 0;
			float dt = (hrt_absolute_time() - last_run) / 1000000.0f;   // calculate the time delta_t between two runs
			last_run = hrt_absolute_time();

			// guard against too small (< 2ms) and too large (> 20ms) dt's
			if (dt < 0.002f) {
				dt = 0.002f;

			} else if (dt > 0.02f) {
				dt = 0.02f;
			}

			// copy position and orientation data
			orb_copy(ORB_ID(vehicle_attitude), _v_att_sub, &_v_att);
            orb_copy(ORB_ID(vehicle_local_position), _v_pos_sub, &_v_pos);
            //sensor_combined_s sensors;
           // orb_copy(ORB_ID(sensor_combined), _sensor_combined_sub, &sensors);
            orb_copy(ORB_ID(sensor_combined), _sensors_sub, &sensors);

			// do path control
            path_control(dt);

			// publish logging data
			if (_logging_hippocampus_pub != nullptr) {
				orb_publish(ORB_ID(logging_hippocampus), _logging_hippocampus_pub, &_logging_hippocampus);

			} else {
				_logging_hippocampus_pub = orb_advertise(ORB_ID(logging_hippocampus), &_logging_hippocampus);
			}


				// publish debug_valueor
			if (_debug_value_pub != nullptr) {
				orb_publish(ORB_ID(debug_value), _debug_value_pub, &_debug_value);

			} else {
				_debug_value_pub = orb_advertise(ORB_ID(debug_value), &_debug_value);
			}

							// publish debug_valueor
			if (_debug_vect_pub != nullptr) {
				orb_publish(ORB_ID(debug_vect), _debug_vect_pub, &_debug_vect);

			} else {
				_debug_vect_pub = orb_advertise(ORB_ID(debug_vect), &_debug_vect);
			}


			// publish actuator timestamps
			_actuators.timestamp = hrt_absolute_time();
			_actuators.timestamp_sample = _v_att.timestamp;

			if (_actuators_0_pub != nullptr) {
				orb_publish(_actuators_id, _actuators_0_pub, &_actuators);
				perf_end(_controller_latency_perf);

			} else if (_actuators_id) {
				_actuators_0_pub = orb_advertise(_actuators_id, &_actuators);
			}


			_actuators_id = ORB_ID(actuator_controls_0);

			// check for parameter updates
			parameter_update_poll();

		}

		perf_end(_loop_perf);
	}

	_control_task = -1;
}

// start function
int HippocampusPathControl::start()
{
	ASSERT(_control_task == -1);        // give a value -1

	// start the control task, performs any specific accounting, scheduler setup, etc.
        _control_task = px4_task_spawn_cmd("path_contr",
					   SCHED_DEFAULT,
					   SCHED_PRIORITY_MAX - 5,
					   5000,
					   (px4_main_t)&HippocampusPathControl::task_main_trampoline,
					   nullptr);

	if (_control_task < 0) {
		warn("task start failed");
		return -errno;
	}

	return OK;
}

// main function
int path_contr_main(int argc, char *argv[])
{
	if (argc < 2) {
                warnx("usage: path_contr {full|attitude|WS|stop|status}");
		return 1;
	}

	// if command is start, then first control if class exists already, if not, allocate new one
	if (!strcmp(argv[1], "full") || !strcmp(argv[1], "attitude")||!strcmp(argv[1], "WS")) {

                if (path_contr::g_control != nullptr) {
			warnx("already running");
			return 1;
		}

		// allocate new class HippocampusPathControl
                path_contr::g_control = new HippocampusPathControl(argv[1]);

		// check if class has been allocated, if not, give back failure
                if (path_contr::g_control == nullptr) {
			warnx("alloc failed");
			return 1;
		}

		// if function start() can not be called, delete instance of HippocampusPathControl and allocate null pointer
                if (OK != path_contr::g_control->start()) {
                        delete path_contr::g_control;
                        path_contr::g_control = nullptr;
			warnx("start failed");
			return 1;
		}

		return 0;
	}

	// if command is start, check if class exists, if not can not stop anything
	if (!strcmp(argv[1], "stop")) {
                if (path_contr::g_control == nullptr) {
			warnx("not running");
			return 1;
		}

		// if class exists, delete it and allocate null pointer
                delete path_contr::g_control;
                path_contr::g_control = nullptr;
		return 0;
	}

	// if command is status and class exists, give back running, else give back not running
	if (!strcmp(argv[1], "status")) {
                if (path_contr::g_control) {
			warnx("running");
			return 0;

		} else {
			warnx("not running");
			return 1;
		}
	}

	warnx("unrecognized command");
	return 1;
}
