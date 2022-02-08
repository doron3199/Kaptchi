import cv2
from kivy.graphics.texture import Texture
from frontend.frontend import MainScreen
"""Get data from the backend to front end and vice versa
For example -> get parameters from sliders, and send to the backend
From the backend it get an openCV image variable and convert it to variable that kivy can display """


class Bus:
    def __init__(self, main_screen: MainScreen):
        self.main_screen = main_screen

    def update_main_image(self, opencv_image):
        """this function get an opencv image, convert it to kivy texture
        and update the main image in the UI"""
        buf1 = cv2.flip(opencv_image, 0)
        buf = buf1.tostring()
        texture = Texture.create(size=(opencv_image.shape[1], opencv_image.shape[0]), colorfmt='bgr')
        # if working on RASPBERRY PI, use colorfmt='rgba' here instead, but stick with "bgr" in blit_buffer.
        texture.blit_buffer(buf, colorfmt='bgr', bufferfmt='ubyte')
        self.main_screen.update_main_image(texture)

