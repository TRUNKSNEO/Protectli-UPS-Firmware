class HwErrors
{
	enum Errors {
		LOAD_OC,
		BATT_OC,
		BATT_OV
	};

	uint8_t error_code;
	uint16_t error_count;     // How many errors until you flag it
	uint16_t cur_error_count; // Current amount of flagged errors
	uint16_t rst_ctr;

      public:
	HwErrors();
	uint8_t check();
	void clear();
};
