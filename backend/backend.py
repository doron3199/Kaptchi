import numpy as np
import cv2
from bus.bus import Bus
from kivy.clock import Clock
from kivy.uix.widget import Widget
import logging

class Backend(Widget):
    def __init__(self, bus: Bus, **kwargs):
        super(Backend, self).__init__(**kwargs)
        self.bus = bus
        self.cap = cv2.VideoCapture(0, cv2.CAP_DSHOW)
        if not self.cap.isOpened():
            logging.error("Cannot open camera")
        Clock.schedule_interval(self.send_video, 1/33.0)

    def send_video(self, dt):
        """send video to the bus"""
        # Capture frame-by-frame
        ret, frame = self.cap.read()
        # if frame is read correctly ret is True
        if not ret:
            logging.error("Can't receive frame")
        self.bus.update_main_image(frame)

    def stop(self):
        """called when the application is closed"""
        self.cap.release()
