#ifndef __FOO_SMART_TEMPO__
#define __FOO_SMART_TEMPO__

// Standard C++ includes
#include <cmath>
#include <cstring>
#include <vector>

// foobar2000 includes (must precede pfc usage)
#include "foobar2000/SDK/foobar2000.h"
#include "foobar2000/helpers/helpers.h"

// foo_smart_tempo includes
#include "resource.h"
#include "globals.h"
#include "guid.h"
#include "preferences.h"

// ─── Locale-independent float formatting ─────────────────────────────────────
// Always uses '.' as the decimal separator, regardless of Windows locale.
[[nodiscard]] inline pfc::string8 format_float_locale(double val, int precision)
{
	pfc::string8 s;
	s << pfc::format_float(val, 0, precision);
	s.replace_char(',', '.');
	return s;
}

// Generate eXtra debug info (uncomment to enable)
//#define X_DEBUG
//#define DEBUG
//#define CONSOLE

inline double mean(const std::vector<double>& mean_list)
{
	double sum = 0;
	int size = (int)mean_list.size();

	for (int i = 0; i < size; i++) sum += mean_list[(size_t)i];
	return (size > 0) ? (sum / size) : 0.0;
}

inline double mode(const std::vector<double>& mode_list, double tolerance)
{
	double mode = 0;

	if (mode_list.size() > 0)
	{
		int current_mode_length = 1;
		int max_mode_length = 1;
		double current_mode_sum = mode_list[0];
		double max_mode_sum = mode_list[0];

		for (size_t i = 1; i < mode_list.size(); i++)
		{
			if (mode_list[i] < mode_list[i - 1] + tolerance &&
				mode_list[i] > mode_list[i - 1] - tolerance)
			{
				current_mode_length++;
				current_mode_sum += mode_list[i];

				if (current_mode_length > max_mode_length)
				{
					max_mode_length = current_mode_length;
					max_mode_sum = current_mode_sum;
				}
			}
			else
			{
				current_mode_length = 1;
				current_mode_sum = mode_list[i];
			}
		}

		mode = max_mode_sum / (double)max_mode_length;
	}

	return mode;
}

#endif // __FOO_SMART_TEMPO__
