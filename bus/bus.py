import cv2 as cv
from kivy.graphics.texture import Texture
from kivy.properties import ObjectProperty

"""Get data from the backend to front end and vice versa
For example -> get parameters from sliders, and send to the backend
From the backend it get an openCV image variable and convert it to variable that kivy can display """


class Bus:
    def __init__(self):
        self.frontend = None
        self.backend = None

    def set(self, frontend, backend):
        self.frontend = frontend
        self.backend = backend

    def update_main_image(self, opencv_image):
        """this function get an opencv image, convert it to kivy texture
        and update the main image in the UI"""
        buf1 = cv.flip(opencv_image, 0)
        buf = buf1.tostring()
        texture = Texture.create(size=(opencv_image.shape[1], opencv_image.shape[0]), colorfmt='bgr')
        # if working on RASPBERRY PI, use colorfmt='rgba' here instead, but stick with "bgr" in blit_buffer.
        texture.blit_buffer(buf, colorfmt='bgr', bufferfmt='ubyte')
        self.frontend.update_main_image(texture)

    def on_change_camera_btn_click(self):
        self.backend.on_change_camera_btn_click()

    def on_zoom_change(self, zoom):
        self.backend.on_zoom_change(zoom)

    def on_whiteboard_filter_btn_click(self):
        self.backend.on_whiteboard_filter_btn_click()

    def on_change_zoom_center(self, x, y):
        self.backend.on_change_zoom_center(x, y)

    def cut_region(self, points):
        self.backend.cut_region(points)

