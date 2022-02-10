from kivy.core.window import Window
from kivy.graphics import Rectangle, Color, Line, Ellipse
from kivy.uix.boxlayout import BoxLayout
from kivy.uix.relativelayout import RelativeLayout
from kivy.properties import NumericProperty, ObjectProperty, ListProperty
from bus.bus import Bus
from kivy.metrics import dp


class MainImageBlock(RelativeLayout):
    bottom_bar_height = NumericProperty(0)
    window_width = NumericProperty(0)
    image_ratio = NumericProperty(0)
    rectangle_start_x = NumericProperty(0)
    rectangle_start_y = NumericProperty(0)
    points = ListProperty([])

    def __init__(self, **kw):
        super().__init__(**kw)
        Window.bind(on_resize=self.on_window_resize)

    def on_window_resize(self, window, width, height):
        self.window_width = width

    def on_touch_down(self, touch):
        if self.is_on_image(touch):
            with self.canvas:
                if len(self.points) >= 4:
                    self.points.clear()
                    self.canvas.clear()
                d = dp(10)
                pos = (touch.x - d / 2, touch.y - d / 2 - self.bottom_bar_height)
                self.points.append(pos)
                self.rectangle_start_x, self.rectangle_start_y = touch.x, touch.y
                Color(1, 0, 0)
                Ellipse(pos=pos, size=(d, d))
                touch.ud['line'] = Line(points=(touch.x, touch.y - self.bottom_bar_height))

    def on_touch_move(self, touch):
        if self.is_on_image(touch):
            self.canvas.clear()
            self.points.clear()
            with self.canvas:
                Color(1, 0, 0, 0.3)
                Rectangle(pos=(self.rectangle_start_x, self.rectangle_start_y - self.bottom_bar_height),
                          size=(touch.x - self.rectangle_start_x, touch.y - self.rectangle_start_y))

            touch.ud['line'].points += [touch.x, touch.y - self.bottom_bar_height]

    def is_on_image(self, touch):
        if self.collide_point(*touch.pos):
            # check up down borders
            image_height = (1 / self.image_ratio) * self.width
            border_y = (self.height - image_height) / 2
            if 0 < border_y < touch.y - self.bottom_bar_height < self.height - border_y:
                return True

            # check side borders
            image_width = self.image_ratio * self.height
            border_x = (self.width - image_width) / 2
            if 0 < border_x < touch.x < self.width - border_x:
                return True
        return False


class Frontend(BoxLayout):
    bus = ObjectProperty(None)
    image_height = NumericProperty(0)
    image_width = NumericProperty(0)

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        Window.bind(on_resize=self.on_window_resize)

    def on_window_resize(self, window, width, height):
        self.ids.main_image_block.bottom_bar_height = self.ids.bottom_bar.height

    def set(self, bus: Bus):
        self.bus = bus

    def on_change_camera_btn_click(self):
        self.bus.on_change_camera_btn_click()

    def on_whiteboard_filter_btn_click(self):
        self.bus.on_whiteboard_filter_btn_click()

    def on_zoom_slider_change(self, widget):
        self.bus.on_zoom_change(widget.value_normalized)

    def update_main_image(self, texture):
        """receives an image from the bus and put in on the image widget"""
        if self.image_width != texture.width or self.image_height != texture.height:
            self.image_height = texture.height
            self.image_width = texture.width
            self.ids.main_image_block.image_ratio = self.image_width / self.image_height
            self.ids.main_image_block.bottom_bar_height = self.ids.bottom_bar.height
        self.ids.main_image.texture = texture
