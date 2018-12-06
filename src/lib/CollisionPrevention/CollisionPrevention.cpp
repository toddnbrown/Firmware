/****************************************************************************
 *
 *   Copyright (c) 2018 PX4 Development Team. All rights reserved.
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
 * @file CollisionPrevention.cpp
 * CollisionPrevention controller.
 *
 */

#include <CollisionPrevention/CollisionPrevention.hpp>
using namespace matrix;
using namespace time_literals;


CollisionPrevention::CollisionPrevention(ModuleParams *parent) :
	ModuleParams(parent)
{

}

CollisionPrevention::~CollisionPrevention()
{
	//unadvertise publishers
	if (_constraints_pub != nullptr) {
		orb_unadvertise(_constraints_pub);
	}

	if (_mavlink_log_pub != nullptr) {
		orb_unadvertise(_mavlink_log_pub);
	}
}

bool CollisionPrevention::initializeSubscriptions(SubscriptionArray &subscription_array)
{
	if (!subscription_array.get(ORB_ID(obstacle_distance), _sub_obstacle_distance)) {
		return false;
	}

	for (unsigned i = 0; i < ORB_MULTI_MAX_INSTANCES; i++) {
		if (!subscription_array.get(ORB_ID(distance_sensor), _sub_distance_sensor[i], i)) {
			return false;
		}
	}

	if (!subscription_array.get(ORB_ID(vehicle_attitude), _sub_vehicle_attitude)) {
		return false;
	}

	return true;
}

void CollisionPrevention::reset_constraints()
{

	_move_constraints_x_normalized.zero();  //normalized constraint in x-direction
	_move_constraints_y_normalized.zero();  //normalized constraint in y-direction

	_move_constraints_x.zero();  //constraint in x-direction
	_move_constraints_y.zero();  //constraint in y-direction
}

void CollisionPrevention::publish_constraints(const Vector2f &original_setpoint, const Vector2f &adapted_setpoint)
{

	collision_constraints_s	constraints;	/**< collision constraints message */

	//fill in values
	constraints.timestamp = hrt_absolute_time();
	constraints.constraints_normalized_x[0] = _move_constraints_x_normalized(0);
	constraints.constraints_normalized_x[1] = _move_constraints_x_normalized(1);
	constraints.constraints_normalized_y[0] = _move_constraints_y_normalized(0);
	constraints.constraints_normalized_y[1] = _move_constraints_y_normalized(1);

	constraints.original_setpoint[0] = original_setpoint(0);
	constraints.original_setpoint[1] = original_setpoint(1);
	constraints.adapted_setpoint[0] = adapted_setpoint(0);
	constraints.adapted_setpoint[1] = adapted_setpoint(1);

	// publish constraints
	if (_constraints_pub != nullptr) {
		orb_publish(ORB_ID(collision_constraints), _constraints_pub, &constraints);

	} else {
		_constraints_pub = orb_advertise(ORB_ID(collision_constraints), &constraints);
	}
}

obstacle_distance_s CollisionPrevention::update_distance_sensor()
{
	obstacle_distance_s obstacle_distance = {};

	for (unsigned i = 0; i < ORB_MULTI_MAX_INSTANCES; i++) {
		const distance_sensor_s &distance_sensor = _sub_distance_sensor[i]->get();

		// consider only instaces with updated data and orientations useful for collision prevention
		if ((hrt_elapsed_time(&distance_sensor.timestamp) < RANGE_STREAM_TIMEOUT_US) && (
			    (distance_sensor.orientation == distance_sensor_s::ROTATION_FORWARD_FACING) ||
			    (distance_sensor.orientation == distance_sensor_s::ROTATION_RIGHT_FACING) ||
			    (distance_sensor.orientation == distance_sensor_s::ROTATION_LEFT_FACING) ||
			    (distance_sensor.orientation == distance_sensor_s::ROTATION_BACKWARD_FACING))) {

			obstacle_distance.timestamp = distance_sensor.timestamp;
			obstacle_distance.max_distance = distance_sensor.max_distance * 100.0f; // convert to cm
			obstacle_distance.min_distance = distance_sensor.min_distance * 100.0f; // convert to cm
			obstacle_distance.increment = 5;

			// init array of distance measuraments
			for (unsigned int k = 0; k < sizeof(obstacle_distance.distances) / sizeof(obstacle_distance.distances[0]); ++k) {
				obstacle_distance.distances[k] = UINT16_MAX;
			}

			// init offset for sensor orientation distance_sensor_s::ROTATION_FORWARD_FACING
			float offset = 0.0f;

			switch (distance_sensor.orientation) {
			case distance_sensor_s::ROTATION_RIGHT_FACING:
				offset = M_PI_F / 2.0f;
				break;

			case distance_sensor_s::ROTATION_LEFT_FACING:
				offset = -M_PI_F / 2.0f;
				break;

			case distance_sensor_s::ROTATION_BACKWARD_FACING:
				offset = M_PI_F;
				break;
			}

			// convert the sensor orientation from body to local frame
			float sensor_orientation = math::degrees(wrap_pi(matrix::Eulerf(matrix::Quatf(_sub_vehicle_attitude->get().q)).psi() +
						   offset));

			// convert orientation from range [-180, 180] to [0, 360]
			if ((sensor_orientation <= FLT_EPSILON) || (sensor_orientation >= 180.0f)) {
				sensor_orientation += 360.0f;
			}

			// array resolution defined by the increment, index 0 is always local north
			const int index = (int)floorf(sensor_orientation / (float)obstacle_distance.increment);

			// compensate measurement for vehicle tilt and convert to cm
			obstacle_distance.distances[index] = 100.0f * distance_sensor.current_distance * cosf(matrix::Eulerf(matrix::Quatf(
					_sub_vehicle_attitude->get().q)).theta());
		}
	}

	return obstacle_distance;
}

void CollisionPrevention::update_range_constraints()
{
	const obstacle_distance_s &obstacle_distance = _sub_obstacle_distance->get();
	obstacle_distance_s distance_data = obstacle_distance;

	// if there aren't distance data from offboard on obstacle_distance, check for onboard sensors on distance_sensor
	if (!_sub_obstacle_distance->updated()) {
		distance_data = update_distance_sensor();
	}

	if (hrt_elapsed_time(&distance_data.timestamp) < RANGE_STREAM_TIMEOUT_US) {
		float max_detection_distance = distance_data.max_distance / 100.0f; //convert to meters

		int distances_array_size = sizeof(distance_data.distances) / sizeof(distance_data.distances[0]);

		for (int i = 0; i < distances_array_size; i++) {
			//determine if distance bin is valid and contains a valid distance measurement
			if (distance_data.distances[i] < distance_data.max_distance &&
			    distance_data.distances[i] > distance_data.min_distance && i * distance_data.increment < 360) {
				float distance = distance_data.distances[i] / 100.0f; //convert to meters
				float angle = math::radians((float)i * distance_data.increment);

				//calculate normalized velocity reductions
				float vel_lim_x = (max_detection_distance - distance) / (max_detection_distance - _param_mpc_col_prev_d.get()) * cos(
							  angle);
				float vel_lim_y = (max_detection_distance - distance) / (max_detection_distance - _param_mpc_col_prev_d.get()) * sin(
							  angle);

				if (vel_lim_x > 0 && vel_lim_x > _move_constraints_x_normalized(1)) { _move_constraints_x_normalized(1) = vel_lim_x; }

				if (vel_lim_y > 0 && vel_lim_y > _move_constraints_y_normalized(1)) { _move_constraints_y_normalized(1) = vel_lim_y; }

				if (vel_lim_x < 0 && -vel_lim_x > _move_constraints_x_normalized(0)) { _move_constraints_x_normalized(0) = -vel_lim_x; }

				if (vel_lim_y < 0 && -vel_lim_y > _move_constraints_y_normalized(0)) { _move_constraints_y_normalized(0) = -vel_lim_y; }
			}
		}

	} else if (_last_message + MESSAGE_THROTTLE_US < hrt_absolute_time()) {
		mavlink_log_critical(&_mavlink_log_pub, "No range data received");
		_last_message = hrt_absolute_time();
	}
}

void CollisionPrevention::modifySetpoint(Vector2f &original_setpoint, const float max_speed)
{
	reset_constraints();

	//calculate movement constraints based on range data
	update_range_constraints();

	//clamp constraints to be in [0,1]. Constraints > 1 occur if the vehicle is closer than _param_mpc_col_prev_d to the obstacle.
	//they would lead to the vehicle being pushed back from the obstacle which we do not yet support
	_move_constraints_x_normalized(0) = math::constrain(_move_constraints_x_normalized(0), 0.f, 1.f);
	_move_constraints_x_normalized(1) = math::constrain(_move_constraints_x_normalized(1), 0.f, 1.f);
	_move_constraints_y_normalized(0) = math::constrain(_move_constraints_y_normalized(0), 0.f, 1.f);
	_move_constraints_y_normalized(1) = math::constrain(_move_constraints_y_normalized(1), 0.f, 1.f);

	//apply the velocity reductions to form velocity limits
	_move_constraints_x(0) = max_speed * (1.f - _move_constraints_x_normalized(0));
	_move_constraints_x(1) = max_speed * (1.f - _move_constraints_x_normalized(1));
	_move_constraints_y(0) = max_speed * (1.f - _move_constraints_y_normalized(0));
	_move_constraints_y(1) = max_speed * (1.f - _move_constraints_y_normalized(1));

	//constrain the velocity setpoint to respect the velocity limits
	Vector2f new_setpoint;
	new_setpoint(0) = math::constrain(original_setpoint(0), -_move_constraints_x(0), _move_constraints_x(1));
	new_setpoint(1) = math::constrain(original_setpoint(1), -_move_constraints_y(0), _move_constraints_y(1));

	//warn user if collision prevention starts to interfere
	bool currently_interfering = (new_setpoint(0) < original_setpoint(0) - 0.05f * max_speed
				      || new_setpoint(0) > original_setpoint(0) + 0.05f * max_speed
				      || new_setpoint(1) < original_setpoint(1) - 0.05f * max_speed
				      || new_setpoint(1) > original_setpoint(1) + 0.05f * max_speed);

	if (currently_interfering && (currently_interfering != _interfering)) {
		mavlink_log_critical(&_mavlink_log_pub, "Collision Warning");
	}

	_interfering = currently_interfering;

	publish_constraints(original_setpoint, new_setpoint);
	original_setpoint = new_setpoint;
}
