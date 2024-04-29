#include <string.h>
#include <stdlib.h>
#include "battery.h"
#include "csd97395.h"

Battery::Battery()
	: bump_amt(0.000001), initial_drive(0.780), _drive(initial_drive),
	  scaling(1)
{
}

float Battery::compute_drive(float v, float i)
{

	if (i < target_i && v < target_v) {
		_drive -= bump_amt; // This adds current
	} else {
		_drive += bump_amt;
	}

	if (_drive >= MAX_DRIVE) {
		_drive = MAX_DRIVE;
	}

	// If the drive have dipped below this amount, something has happened.
	if (_drive <= MIN_DRIVE) {
		_drive = initial_drive;
	}

	return _drive;
}

#define I_TSH 100
#define V_TSH 0.2

float Battery::compute_drive(float v, float i, float drive)
{
	float tg_i = target_i * scaling;

	if ((abs(i - tg_i) < I_TSH) || abs(v - target_v) < V_TSH) {
		// Do nothing
	} else if (i < tg_i && v < target_v) {
		drive -= bump_amt; // This adds current
	} else {
		drive += bump_amt;
	}

	if (drive >= MAX_DRIVE) {
		drive = MAX_DRIVE;
	}

	// If the drive have dipped below this amount, something has happened.
	if (drive <= MIN_DRIVE) {
		drive = initial_drive;
	}

	return drive;
}
