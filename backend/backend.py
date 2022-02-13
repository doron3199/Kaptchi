import numpy as np
import cv2 as cv
from bus.bus import Bus
from kivy.clock import Clock
from kivy.uix.widget import Widget
import logging
from backend.image_processing import WhiteboardFilter

HIGH_VALUE = 10000
ZOOM_VALUE_FULL = 0.5


class Backend(Widget):
    def __init__(self, **kwargs):
        super(Backend, self).__init__(**kwargs)
        self.bus = None
        self.cap = None
        self.ports = self.list_ports()
        self.port_num = 0
        self.zoom = 0.5
        self.zoom_center_x = 0
        self.zoom_center_y = 0
        self.is_whiteboard_filter_on = False
        self.image_processing = WhiteboardFilter()

    def set(self, bus: Bus):
        self.bus = bus
        self.start()

    def start(self):
        self.cap = cv.VideoCapture(self.ports[self.port_num], cv.CAP_DSHOW)
        # HIGH_VALUE set the highest resolution of the camera, even if it not the actual resolution
        self.cap.set(cv.CAP_PROP_FRAME_WIDTH, HIGH_VALUE)
        self.cap.set(cv.CAP_PROP_FRAME_HEIGHT, HIGH_VALUE)
        if not self.cap.isOpened():
            logging.error("Cannot open camera")
        Clock.schedule_interval(self.send_video, 1 / 33.0)

    def send_video(self, dt):
        """send video to the bus"""
        # Capture frame-by-frame
        ret, frame = self.cap.read()
        # if frame is read correctly ret is True
        if not ret:
            logging.error("Can't receive frame")

        zoomed_image = self.zoom_image(frame)
        if self.is_whiteboard_filter_on:
            zoomed_image = self.image_processing.clean_image(zoomed_image)
        self.bus.update_main_image(zoomed_image)

    def on_change_zoom_center(self, x, y):
        self.zoom_center_x += int(x * self.zoom * 9)
        self.zoom_center_y += int(-y * self.zoom * 9)

    def zoom_image(self, image):
        """zoom image, zoom factor - how much zoom, zoom_center_x and zoom_center_y are for where to zoom in the
        image"""
        h, w = image.shape[0:2]
        if self.zoom_center_x == 0 or self.zoom_center_y == 0 or self.zoom == ZOOM_VALUE_FULL:
            self.zoom_center_x = w // 2
            self.zoom_center_y = h // 2
        height_crop = int(h * self.zoom)
        width_crop = int(w * self.zoom)
        self.zoom_center_x = min(max(self.zoom_center_x, width_crop), w - width_crop)
        self.zoom_center_y = min(max(self.zoom_center_y, height_crop), h - height_crop)
        cropped = image[self.zoom_center_y - height_crop: self.zoom_center_y + height_crop,
                        self.zoom_center_x - width_crop: self.zoom_center_x + width_crop]
        return cv.resize(cropped, (w, h))

    def on_change_camera_btn_click(self):
        self.port_num = (self.port_num + 1) % len(self.ports)
        self.cap = cv.VideoCapture(self.ports[self.port_num], cv.CAP_DSHOW)
        # HIGH_VALUE set the highest resolution of the camera, even if it not the actual resolution
        self.cap.set(cv.CAP_PROP_FRAME_WIDTH, HIGH_VALUE)
        self.cap.set(cv.CAP_PROP_FRAME_HEIGHT, HIGH_VALUE)

    def stop(self):
        """called when the application is closed"""
        self.cap.release()

    def on_whiteboard_filter_btn_click(self):
        self.is_whiteboard_filter_on = not self.is_whiteboard_filter_on

    def on_zoom_change(self, zoom):
        """we receive the zoom value as percentage, set it as a factor of magnification. multiply by
        0.99 because zoom = 0 will break the program. device by 2 because we do it from the middle"""
        self.zoom = (1 - zoom * 0.99) / 2

    def list_ports(self):
        """
        Test the ports and returns a tuple with the available ports and the ones that are working.
        """
        non_working_ports = []
        dev_port = 0
        working_ports = []
        available_ports = []
        while len(non_working_ports) < 3:  # if there are more than 2 non working ports stop the testing.
            camera = cv.VideoCapture(dev_port, cv.CAP_DSHOW)
            if not camera.isOpened():
                non_working_ports.append(dev_port)
            else:
                is_reading, img = camera.read()
                if is_reading:
                    working_ports.append(dev_port)
                else:
                    available_ports.append(dev_port)
            dev_port += 1
        return working_ports
