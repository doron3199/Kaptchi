import kivy
from kivy.config import Config
Config.set('input', 'mouse', 'mouse,multitouch_on_demand')
kivy.require('2.0.0')
from kivy.app import App
from frontend.frontend import Frontend
from bus.bus import Bus
from backend.backend import Backend
from kivy.lang import Builder
import os

class KaptchiApp(App):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.backend = None

    def build(self):
        Builder.load_file(os.path.join('frontend','kaptchi.kv'))
        frontend = Frontend()
        bus = Bus()
        self.backend = Backend()

        bus.set(backend=self.backend, frontend=frontend)
        self.backend.set(bus)
        frontend.set(bus)

        return frontend

    def on_stop(self):
        self.backend.stop()


if __name__ == '__main__':
    KaptchiApp().run()
