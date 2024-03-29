#include <zephyr/kernel.h>

class Battery
{
	int target_v; // mV
	float bump_amt;
	float initial_drive;
	float _drive;
	float target_i; // mA
	float scaling;

      public:
	Battery();
	float compute_drive(float v, float i);
	float compute_drive(float v, float i, float drive);

	Battery &set_voltage(float v)
	{
		target_v = v * 1000;
		return *this;
	}

	Battery &set_current(float i)
	{
		target_i = i;
		return *this;
	}

	Battery &set_scaling(float s)
	{
		scaling = s;
		return *this;
	}
};
