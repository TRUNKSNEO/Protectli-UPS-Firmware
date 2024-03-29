#include <string.h>
#include "battery.h"

Battery::Battery()
	: bump_amt(0.000001), initial_drive(0.780), _drive(initial_drive)
{
}

float Battery::compute_drive(float v, float i)
{

	if (i < target_i && v < target_v) {
		_drive -= bump_amt; // This adds current
	} else {
		_drive += bump_amt;
	}

	if (_drive >= 0.85) {
		_drive = 0.85;
	}

	// If the drive have dipped below this amount, something has happened.
	if (_drive <= 0.65) {
		_drive = initial_drive;
	}

	return _drive;
}

float Battery::compute_drive(float v, float i, float drive)
{

	if (i < target_i && v < target_v) {
		drive -= bump_amt; // This adds current
	} else {
		drive += bump_amt;
	}

	if (drive >= 0.85) {
		drive = 0.85;
	}

	// If the drive have dipped below this amount, something has happened.
	if (drive <= 0.65) {
		drive = initial_drive;
	}

	return drive;
}
