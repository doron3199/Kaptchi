from kivy.uix.boxlayout import BoxLayout


class MainScreen(BoxLayout):
    def update_main_image(self, texture):
        self.ids.main_image.texture = texture



