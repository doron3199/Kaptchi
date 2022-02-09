import numpy as np
import cv2
from bus.bus import Bus
from kivy.clock import Clock
from kivy.uix.widget import Widget
import logging


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

    def set(self, bus: Bus):
        self.bus = bus
        self.start()

    def start(self):
        self.cap = cv2.VideoCapture(self.ports[self.port_num], cv2.CAP_DSHOW)
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
        self.bus.update_main_image(zoomed_image)

    def zoom_image(self, image):
        """zoom image, zoom factor - how much zoom, zoom_center_x and zoom_center_y are for where to zoom in the
        image"""
        h, w = image.shape[0:2]
        self.zoom_center_x = w // 2
        self.zoom_center_y = h // 2
        height_crop = int(h * self.zoom)
        width_crop = int(w * self.zoom)
        cropped = image[self.zoom_center_y - height_crop: self.zoom_center_y + height_crop,
                  self.zoom_center_x - width_crop:self.zoom_center_x + width_crop]
        return cv2.resize(cropped, None, fx=h / cropped.shape[0], fy=w / cropped.shape[1])

    def on_change_camera_btn_click(self):
        self.port_num = (self.port_num + 1) % len(self.ports)
        self.cap = cv2.VideoCapture(self.ports[self.port_num], cv2.CAP_DSHOW)

    def stop(self):
        """called when the application is closed"""
        self.cap.release()

    def on_zoom_change(self, zoom):
        """we receive the zoom value as percentage, set it as a factor of magnification
        device by 2 because we do it from the middle"""
        self.zoom = (1 - zoom)/2

    def list_ports(self):
        """
        Test the ports and returns a tuple with the available ports and the ones that are working.
        """
        non_working_ports = []
        dev_port = 0
        working_ports = []
        available_ports = []
        while len(non_working_ports) < 3:  # if there are more than 2 non working ports stop the testing.
            camera = cv2.VideoCapture(dev_port, cv2.CAP_DSHOW)
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
