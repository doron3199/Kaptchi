import cv2 as cv
from kivy.graphics.texture import Texture

"""Get data from the backend to front end and vice versa
For example -> get parameters from sliders, and send to the backend
From the backend it get an openCV image variable and convert it to variable that kivy can display """


class Bus:
    def __init__(self):
        self._frontend = None
        self._backend = None

    def set(self, frontend, backend):
        self._frontend = frontend
        self._backend = backend

    def update_main_image(self, opencv_image):
        """this function get an opencv image, convert it to kivy texture
        and update the main image in the UI"""
        buf1 = cv.flip(opencv_image, 0)
        buf = buf1.tostring()
        texture = Texture.create(size=(opencv_image.shape[1], opencv_image.shape[0]), colorfmt='bgr')
        # if working on RASPBERRY PI, use colorfmt='rgba' here instead, but stick with "bgr" in blit_buffer.
        texture.blit_buffer(buf, colorfmt='bgr', bufferfmt='ubyte')
        self._frontend.update_main_image(texture)

    def on_change_camera_btn_click(self):
        self._backend.on_change_camera_btn_click()

    def on_zoom_change(self, zoom):
        self._backend.on_zoom_change(zoom)

    def on_whiteboard_filter_btn_click(self):
        self._backend.on_whiteboard_filter_btn_click()

    def on_change_zoom_center(self, x, y):
        self._backend.on_change_zoom_center(x, y)

    def cut_region(self, points):
        self._backend.cut_region(points)

    def on_video_link_btn_click(self, value):
        self._backend.on_video_link_btn_click(value)

    def set_video_bar(self, value):
        self._frontend.set_video_bar(value)

    def set_video_time(self, value):
        self._backend.set_video_time(value)

    def update_video_slider(self, value):
        self._frontend.update_video_slider(value)

    def play_pause(self, value=None):
        self._backend.play_pause(value)
