#pragma once
#include <string>
#include <array>
#include <vector>

#include "Etterna/Models/NoteData/NoteDataStructures.h"

// ok i remember now its because i wanted to smooth the mod before applying to
// cj
struct CJDensityMod
{
	const CalcPatternMod _pmod = CJDensity;
	const std::string name = "CJDensityMod";
	const int _tap_size = quad;

#pragma region params

	float min_mod = 0.9F;
	float max_mod = 1.3F;
	float base = 0.1F;

	float jump_scaler = 1.F;
	float hand_scaler = 1.33F;
	float quad_scaler = 2.F;

	const vector<pair<std::string, float*>> _params{
		{ "min_mod", &min_mod },
		{ "max_mod", &max_mod },
		{ "base", &base },

		{ "jump_scaler", &jump_scaler },
		{ "hand_scaler", &hand_scaler },
		{ "quad_scaler", &quad_scaler },
	};
#pragma endregion params and param map

	float pmod = neutral;

	inline auto operator()(const metaItvInfo& mitvi) -> float
	{
		const auto& itvi = mitvi._itvi;
		if (itvi.total_taps == 0) {
			return neutral;
		}

		float t_taps = static_cast<float>(itvi.total_taps);
		float a1 =
		  static_cast<float>(static_cast<float>(itvi.taps_by_size[jump]) *
							 jump_scaler) /
		  t_taps;
		float a2 =
		  static_cast<float>(static_cast<float>(itvi.taps_by_size[hand]) *
							 hand_scaler) /
		  t_taps;
		float a3 =
		  static_cast<float>(static_cast<float>(itvi.taps_by_size[quad]) *
							 quad_scaler) /
		  t_taps;

		float aaa = a1 + a2 + a3;

		pmod = CalcClamp(base + fastsqrt(aaa), min_mod, max_mod);

		return pmod;
	}
};
