"""Weather on the existing per-link delivery worker, independent of agent cards."""

import asyncio

from orb_hub.lifecycle import LifecycleSupervisor
from orb_hub.weather import WeatherLocation, WeatherProvider


class WeatherBridge:
    def __init__(self, sessions, location=None, *, provider=None):
        self.sessions = sessions
        self.provider = provider or WeatherProvider(location)
        self._wake = asyncio.Event()
        self._worker = None
        self.lifecycle = LifecycleSupervisor("weather")

    async def start(self):
        await self.provider.start()
        self.sessions.set_weather_provider(self.snapshot)
        self._worker = self.lifecycle.run(self._run, name="orb-weather", restarts=2)

    async def stop(self):
        await self.lifecycle.stop()
        self._worker = None
        self.sessions.set_weather_provider(None)
        await self.provider.stop()

    def snapshot(self, device_id):
        unit = self.sessions.get_prefs(device_id)["units"]["temperature"]
        return self.provider.snapshot(unit)

    def subscribe(self, link):
        self.sessions.subscribe_weather(link)
        self._wake.set()

    def set_location(self, location: dict | None):
        self.provider.set_location(WeatherLocation(**location) if location else None)
        # Publish loading/off before awaiting a remote request, so clearing the
        # setting cannot leave the previous city's reading visible.
        self.publish()
        self._wake.set()

    def publish(self):
        self.sessions.publish_weather()

    async def _run(self):
        while True:
            self._wake.clear()
            units = self.sessions.weather_units()
            await self.provider.tick(units)
            self.publish()
            try:
                async with asyncio.timeout(1):
                    await self._wake.wait()
            except TimeoutError:
                pass
