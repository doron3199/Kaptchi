import kivy

kivy.require('2.0.0')
from kivy.app import App
from frontend.frontend import MainScreen
from bus.bus import Bus
from backend.backend import Backend
from kivy.lang import Builder

class KaptchiApp(App):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.backend = None

    def build(self):
        Builder.load_file('frontend/kaptchi.kv')
        frontend = MainScreen()
        bus = Bus(frontend)
        self.backend = Backend(bus)
        return frontend

    def on_stop(self):
        self.backend.stop()


if __name__ == '__main__':
    KaptchiApp().run()
