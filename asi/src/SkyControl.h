// Minecraft's /time and /weather commands set GTA IV's clock and weather (proto::kEvSetTime,
// kEvSetWeather), and GTA IV's weather goes to Minecraft in SkyState's flags (kSkyWeatherShift), so
// Minecraft's weather follows GTA IV's (its own cycle stops while linked: fabric world/HostSky.java).
//  - kEvSetTime: SET_TIME_OF_DAY at the hour and minute Minecraft's time stands for (tick 0 = 06:00).
//  - kEvSetWeather: FORCE_WEATHER_NOW of the type for as many seconds as Minecraft's command said, then
//    RELEASE_WEATHER: GTA IV's own weather cycle again.
#pragma once

#include "LinkCore.h"

#include <cstdint>

namespace lc::SkyControl
{
	namespace proto = ::libertycraft::proto;

	// Game thread: a kEvSetTime or kEvSetWeather from Minecraft.
	void OnEvent(const proto::McEvent& a_ev);

	// Game thread, once per frame: counts a forced weather's time down. a_playing: in game, not loading.
	void Tick(float a_dt, bool a_playing);

	// GTA IV's weather for SkyState::flags (already shifted: kSkyWeatherShift; 0 if unknown).
	std::uint32_t WeatherBits();
}
