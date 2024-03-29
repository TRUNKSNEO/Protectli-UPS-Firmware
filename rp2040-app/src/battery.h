#include <zephyr/kernel.h>

class Battery
{
	int target_v;   // mV
	float bump_amt;
	float initial_drive;
	float _drive;
	float target_i; // mA

      public:
	Battery();
	float compute_drive(float v, float i);
	float compute_drive(float v, float i, float drive);

	Battery &setVoltage(float v)
	{
		target_v = v * 1000;
		return *this;
	}

	Battery &setCurrent(float i)
	{
		target_i = i;
		return *this;
	}
};
