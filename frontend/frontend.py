from kivy.graphics import Rectangle, Color, Line, Ellipse
from kivy.uix.boxlayout import BoxLayout
from kivy.uix.relativelayout import RelativeLayout
from kivy.properties import NumericProperty, ObjectProperty, ListProperty, BooleanProperty, StringProperty
from bus.bus import Bus
from kivy.metrics import dp


class VideoSlider(BoxLayout):
    video_length_in_seconds = NumericProperty(0)
    video_slider_value = NumericProperty(0)

    def set_video_length_in_seconds(self, value):
        self.ids.video_slider.max = value

    def set_video_slider_value(self, value):
        self.ids.video_slider.value = value

    def on_touch_down(self, touch):
        super(VideoSlider, self).on_touch_down(touch)
        if self.collide_point(*touch.pos):
            self.parent.parent.parent.bus.play_pause(False)

    def on_touch_up(self, touch):
        super(VideoSlider, self).on_touch_up(touch)
        if self.collide_point(*touch.pos):
            self.parent.parent.parent.bus.set_video_time(self.ids.video_slider.value)
            if self.parent.parent.parent.play:
                self.parent.parent.parent.bus.play_pause(True)


class MainImageBlock(RelativeLayout):
    bottom_bar_height = NumericProperty(0)
    image_ratio = NumericProperty(0)
    rectangle_start_x = NumericProperty(0)
    rectangle_start_y = NumericProperty(0)
    points = ListProperty([])
    is_cut_region = BooleanProperty(False)
    zoom_center_start_x = NumericProperty(0)
    zoom_center_start_y = NumericProperty(0)

    def on_touch_down(self, touch):
        if self.is_on_image(touch):

            if touch.is_mouse_scrolling:
                if touch.button == 'scrolldown':
                    self.parent.parent.ids.zoom.value = min(self.parent.parent.ids.zoom.value + 5, 100)
                elif touch.button == 'scrollup':
                    self.parent.parent.ids.zoom.value = max(self.parent.parent.ids.zoom.value - 5, 1)
            elif touch.button == 'right':
                self.canvas.clear()
                self.points.clear()
                self.parent.parent.is_cut_region_disabled = True
            elif touch.is_double_tap:
                with self.canvas:
                    if len(self.points) >= 4:
                        self.points.clear()
                        self.canvas.clear()
                    d = dp(10)
                    pos = (touch.x - d / 2, touch.y - d / 2 - self.bottom_bar_height)
                    self.points.append((touch.x, touch.y - self.bottom_bar_height))
                    self.rectangle_start_x, self.rectangle_start_y = touch.x, touch.y
                    Color(1, 1, 0, 0.5)
                    Ellipse(pos=pos, size=(d, d))
                    if len(self.points) == 4:
                        self.parent.parent.is_cut_region_disabled = False
                    else:
                        self.parent.parent.is_cut_region_disabled = True

            else:
                self.zoom_center_start_x = touch.x
                self.zoom_center_start_y = touch.y

    def on_is_cut_region(self, _, state):
        self.parent.parent.bus.cut_region(self.canvas_to_image_coordinates(self.points))
        self.canvas.clear()
        self.points.clear()
        if not state:
            self.parent.parent.is_cut_region_disabled = True

    def on_touch_move(self, touch):
        if self.is_on_image(touch):
            if touch.is_double_tap:
                self.canvas.clear()
                self.points.clear()
                self.points = self.rect_to_four_points(
                    (self.rectangle_start_x, self.rectangle_start_y - self.bottom_bar_height),
                    (touch.x, touch.y - self.bottom_bar_height))
                with self.canvas:
                    Color(1, 0, 0, 0.3)
                    Rectangle(pos=(self.rectangle_start_x, self.rectangle_start_y - self.bottom_bar_height),
                              size=(touch.x - self.rectangle_start_x, touch.y - self.rectangle_start_y))
                    self.parent.parent.is_cut_region_disabled = False
            else:
                # by resetting the start we measure the "speed" and not the distance, because if we measure the
                # distance, if we're going with the mouse to one way and then turning back a little it still positive
                # distance, and we will go to the wrong side
                if self.parent.parent.frame_counter % 2 == 0:
                    self.zoom_center_start_x = touch.x
                    self.zoom_center_start_y = touch.y
                self.parent.parent.bus.on_change_zoom_center(self.zoom_center_start_x - touch.x,
                                                             self.zoom_center_start_y - touch.y)

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

    def canvas_to_image_coordinates(self, points):
        """ this function get a list of points with coordinates from the image widget and convert it to
        a list of points that looks like in the same place, but in the real image coordinates"""
        # just make it more readable
        image_height = self.parent.parent.image_height
        image_width = self.parent.parent.image_width
        # calc up-down borders
        widget_image_height = (1 / self.image_ratio) * self.width
        border_y = max(0, (self.height - widget_image_height) / 2)
        # calc side borders
        widget_image_width = self.image_ratio * self.height
        border_x = max(0, (self.width - widget_image_width) / 2)

        # where are the borders?
        if border_y == 0:
            # side borders
            widget_height = self.height
            widget_width = self.width - border_x * 2
        else:
            # up-down borders
            widget_height = self.height - border_y * 2
            widget_width = self.width

        final_points = []
        # change it to numpy matrix multiplication for performance increase,
        # but it will be at most 4 points, so it will be pointless! haha! get it? I am lonely
        for point in points:
            # just in case its a tuple
            point = list(point)
            point[0] -= border_x
            point[1] -= border_y

            # calc the borders by the unit percentage
            width = (point[0] / widget_width) * image_width
            # wait why minus? because kivy 0 y is bottom and in opencv 0 y is top
            height = image_height - (point[1] / widget_height) * image_height
            final_points.append((int(width), int(height)))
        return final_points

    def rect_to_four_points(self, a, b):
        """get two points of a rectangle, a is upper left, b it bottom right, return the rectangle edges"""
        return (a[0], a[1]), (a[0], b[1]), (b[0], a[1]), (b[0], b[1])


class Frontend(BoxLayout):
    is_cut_region_disabled = BooleanProperty(True)
    bus = ObjectProperty(None)
    image_height = NumericProperty(0)
    image_width = NumericProperty(0)
    frame_counter = NumericProperty(0)
    video_link_value = StringProperty("")
    pick_button_disabled = BooleanProperty(True)
    is_video_player = BooleanProperty(False)
    play = BooleanProperty(True)

    def set(self, bus: Bus):
        self.bus = bus

    def on_change_camera_btn_click(self):
        self.is_video_player = False
        self.bus.on_change_camera_btn_click()

    def on_whiteboard_filter_btn_click(self):
        self.bus.on_whiteboard_filter_btn_click()

    def on_remove_foreground_btn_click(self):
        self.bus.on_remove_foreground_btn_click()

    def on_cut_region_btn_click(self):
        self.ids.main_image_block.is_cut_region = not self.ids.main_image_block.is_cut_region

    def on_zoom_slider_change(self, widget):
        self.bus.on_zoom_change(widget.value_normalized)

    def update_main_image(self, texture):
        self.frame_counter = (self.frame_counter + 1) % 30
        """receives an image from the bus and put in on the image widget"""
        if self.image_width != texture.width or self.image_height != texture.height:
            self.image_height = texture.height
            self.image_width = texture.width
            self.ids.main_image_block.image_ratio = self.image_width / self.image_height
        if self.ids.main_image_block.bottom_bar_height != self.ids.bottom_bar.height:
            self.ids.main_image_block.bottom_bar_height = self.ids.bottom_bar.height
        self.ids.main_image.texture = texture

    def on_video_link_value(self, value):
        # TODO: check if its a link with regex
        self.pick_button_disabled = value == ""

    def update_video_slider(self, video_second):
        self.ids.video_bar.set_video_slider_value(video_second)

    def on_pick_video_btn_click(self, value):
        self.is_video_player = True
        self.bus.on_video_link_btn_click(value)

    def set_video_bar(self, length_in_seconds):
        self.ids.video_bar.set_video_length_in_seconds(length_in_seconds)

    def on_play_pause_btn_press(self):
        self.bus.play_pause()
        self.play = not self.play
