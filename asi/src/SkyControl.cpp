// Unity-built into dllmain.cpp (needs IV-SDK). See SkyControl.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "sky"
#include "SkyControl.h"

#include "Log.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace lc::SkyControl
{
	namespace
	{
		namespace S = ::Scripting;

		const char* WeatherName(std::uint32_t a_type)
		{
			static constexpr const char* kNames[] = { "EXTRASUNNY", "SUNNY", "SUNNY_WINDY", "CLOUDY", "RAIN", "DRIZZLE", "FOGGY", "LIGHTNING" };
			return a_type < 8 ? kNames[a_type] : "?";
		}

		bool  forced = false;
		float forcedLeft = 0.0f;  // seconds; <= 0 with forced: until told otherwise
		std::uint32_t forcedType = 0;
	}

	void OnEvent(const proto::McEvent& a_ev)
	{
		if (a_ev.type == proto::kEvSetTime) {
			const float hour = std::fmod(std::fmod(a_ev.a, 24.0f) + 24.0f, 24.0f);
			const int   h = std::clamp(static_cast<int>(hour), 0, 23);
			const int   m = std::clamp(static_cast<int>((hour - static_cast<float>(h)) * 60.0f), 0, 59);
			int         oldH = 0, oldM = 0;
			S::GET_TIME_OF_DAY(&oldH, &oldM);
			S::SET_TIME_OF_DAY(static_cast<unsigned>(h), static_cast<unsigned>(m));
			LC_LOG("Minecraft set the time: GTA IV's clock %02d:%02d -> %02d:%02d", oldH, oldM, h, m);
			return;
		}
		if (a_ev.type == proto::kEvSetWeather) {
			const std::uint32_t type = std::min<std::uint32_t>(a_ev.formId, proto::kGtaLightning);
			const std::uint32_t before = CWeather::InterpolationValue >= 0.5f ? CWeather::NewWeatherType : CWeather::OldWeatherType;
			S::FORCE_WEATHER_NOW(type);
			forced = true;
			forcedType = type;
			forcedLeft = a_ev.a > 0.0f && std::isfinite(a_ev.a) ? a_ev.a : 0.0f;
			LC_LOG("Minecraft set the weather: GTA IV's %s -> %s, %s", WeatherName(before), WeatherName(type),
				forcedLeft > 0.0f ? (std::to_string(static_cast<int>(forcedLeft)) + " s, then GTA IV's own weather again").c_str() : "until told otherwise");
		}
	}

	void Tick(float a_dt, bool a_playing)
	{
		if (!forced || !a_playing || forcedLeft <= 0.0f) {
			return;
		}
		if ((forcedLeft -= a_dt) <= 0.0f) {
			forced = false;
			S::RELEASE_WEATHER();
			LC_LOG("the %s Minecraft asked for is over: GTA IV's own weather again", WeatherName(forcedType));
		}
	}

	std::uint32_t WeatherBits()
	{
		const std::uint32_t type = CWeather::InterpolationValue >= 0.5f ? CWeather::NewWeatherType : CWeather::OldWeatherType;
		return type < 15 ? (type + 1u) << proto::kSkyWeatherShift : 0u;
	}
}
