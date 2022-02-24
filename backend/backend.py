import numpy as np
import cv2 as cv
from bus.bus import Bus
from kivy.clock import Clock
from kivy.uix.widget import Widget
import logging
from pytube import YouTube
import os
import pathlib
from PIL import Image as PIL_Image
from imutils.video import FileVideoStream
from backend.image_processing import WhiteboardFilter
from backend.transfrom import four_point_transform
from queue import Queue

HIGH_VALUE = 10000
ZOOM_VALUE_FULL = 0.5
GENERAL_FPS = 30.0
NUMBER_OF_PARTS = 10
AUTO_SAVE_QUEUE_SIZE = int(GENERAL_FPS * 3)

class Backend(Widget):
    def __init__(self, **kwargs):
        super(Backend, self).__init__(**kwargs)
        self.raw_videos_path = ''
        self.edited_videos_path = ''
        self.output_path = ''
        self.database_init()
        self.bus = None
        self.cap = None
        self.ports = self.list_ports()
        self.port_num = 0
        self.zoom = 0.5
        self.zoom_center_x = 0
        self.zoom_center_y = 0
        self.is_whiteboard_filter_on = False
        self.image_processing = WhiteboardFilter()
        self.points_to_cut = []
        self.loop_func = None
        self.second_in_video = 0
        self.fps = 0
        self.play = True
        self.slider_time = 0
        self.is_remove_foreground_on = False
        self.fgbg = cv.createBackgroundSubtractorKNN()
        self.fgbg.setHistory(300)
        self.parts = []
        self.final_image = None
        self.current_frame = None
        self.saved_image_dict = {}
        self.image_counter = 0
        self.is_auto_save = False
        self.auto_save_cache_images = Queue(maxsize=AUTO_SAVE_QUEUE_SIZE)
        self.auto_save_cache_averages = Queue(maxsize=AUTO_SAVE_QUEUE_SIZE)
        self.auto_save_counter = 0

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
        self.loop_func = Clock.schedule_interval(self.send_video, 1 / GENERAL_FPS)

    def send_video(self, dt):
        """send video to the bus"""
        if self.play:
            # Capture frame-by-frame
            if isinstance(self.cap, FileVideoStream):
                if not self.cap.more():
                    self.loop_func.cancel()
                    return
                frame = self.cap.read()
                # you can also just count it, maybe better performance?
                self.second_in_video = int(self.cap.stream.get(cv.CAP_PROP_POS_MSEC) / 1000)
                s = self.second_in_video
            else:
                ret, frame = self.cap.read()
                if not ret:
                    self.loop_func.cancel()
                    return
            # if frame is read correctly ret is True
            if self.is_remove_foreground_on:
                frame = self.remove_foreground(frame)
                removed_foreground = frame
            else:
                # then there is still a background picture to use
                removed_foreground = self.remove_foreground(frame)
            frame = self.zoom_image(frame)
            if self.is_whiteboard_filter_on:
                frame = self.image_processing.clean_image(frame)
            if len(self.points_to_cut) == 4:
                frame = four_point_transform(frame, self.points_to_cut)
            self.current_frame = frame
            if self.is_auto_save:
                if self.is_whiteboard_filter_on:
                    self.auto_save(frame)
                else:
                    self.auto_save(self.image_processing.clean_image(removed_foreground))
            self.bus.update_main_image(frame)
            self.bus.update_video_slider(self.second_in_video)

    def remove_foreground(self, image):
        fgmask = self.fgbg.apply(image)
        h, w = image.shape[0:2]
        dist = np.linspace(0, w, NUMBER_OF_PARTS, dtype=int)
        if self.final_image is None:
            self.final_image = image.copy()
        for i in range(NUMBER_OF_PARTS - 1):
            still = not (np.average(fgmask[:, dist[i]:dist[i + 1]]))
            self.final_image[:, dist[i]:dist[i + 1]] = \
                np.multiply(image[:, dist[i]:dist[i + 1]], still) + \
                np.multiply(self.final_image[:, dist[i]:dist[i + 1]], not still)
        return self.final_image

    def cut_region(self, cut_region):
        self.points_to_cut = np.array(cut_region)

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
        self.final_image = None
        self.port_num = (self.port_num + 1) % len(self.ports)
        self.cap = cv.VideoCapture(self.ports[self.port_num], cv.CAP_DSHOW)
        # HIGH_VALUE set the highest resolution of the camera, even if it not the actual resolution
        self.cap.set(cv.CAP_PROP_FRAME_WIDTH, HIGH_VALUE)
        self.cap.set(cv.CAP_PROP_FRAME_HEIGHT, HIGH_VALUE)
        self.loop_func.cancel()
        self.loop_func = Clock.schedule_interval(self.send_video, 1 / GENERAL_FPS)

    def on_video_link_btn_click(self, link):
        self.final_image = None
        if 'www.youtube.com' in link:
            yt = YouTube(link)
            video = yt.streams.get_highest_resolution()
            self.cap = FileVideoStream(video.url).start()
            self.loop_func.cancel()
            self.fps = video.fps
            self.loop_func = Clock.schedule_interval(self.send_video, 1 / self.fps)
            video_length_in_seconds = yt.length
            self.bus.set_video_bar(video_length_in_seconds)
        else:
            self.cap = FileVideoStream(link).start()
            video_length_in_seconds = int(self.cap.stream.get(
                cv.CAP_PROP_FRAME_COUNT) / self.cap.stream.get(cv.CAP_PROP_FPS))
            self.bus.set_video_bar(video_length_in_seconds)
            self.loop_func.cancel()
            self.loop_func = Clock.schedule_interval(self.send_video, 1 / GENERAL_FPS)

    def set_video_time(self, value):
        with self.cap.Q.mutex:
            self.cap.Q.queue.clear()
            self.cap.stream.set(cv.CAP_PROP_POS_MSEC, value * 1000)

    def database_init(self):
        parent_path = pathlib.Path(__file__).parent.parent.resolve()
        database_path = os.path.join(parent_path, 'database')
        if not os.path.isdir(database_path):
            os.mkdir(database_path)
            for folder in ['raw_videos', 'edited_videos', 'output']:
                os.mkdir(os.path.join(database_path, folder))
        for folder in ['raw_videos', 'edited_videos', 'output']:
            folder_path = os.path.join(database_path, folder)
            if not os.path.isdir(folder_path):
                os.mkdir(folder_path)
            self.__setattr__(f'{folder}_path', folder_path)

    def stop(self):
        """called when the application is closed"""
        if isinstance(self.cap, FileVideoStream):
            self.cap.stopped = True
        else:
            self.cap.release()

    def on_whiteboard_filter_btn_click(self):
        self.is_whiteboard_filter_on = not self.is_whiteboard_filter_on

    def on_remove_foreground_btn_click(self):
        self.is_remove_foreground_on = not self.is_remove_foreground_on

    def on_zoom_change(self, zoom):
        """we receive the zoom value as percentage, set it as a factor of magnification. multiply by
        0.99 because zoom = 0 will break the program. device by 2 because we do it from the middle"""
        self.zoom = (1 - zoom * 0.99) / 2

    def play_pause(self, value=None):
        if value is None:
            self.play = not self.play
        else:
            self.play = value

    def on_shot_btn_click(self):
        self.saved_image_dict[str(self.image_counter)] = self.current_frame
        self.bus.add_saved_image(self.current_frame, self.image_counter)
        self.image_counter += 1

    def delete_image_btn_press(self, image_id):
        self.saved_image_dict.pop(str(image_id))

    def export_as_pdf_btn_click(self):
        pil_list = []

        for image in self.saved_image_dict.values():
            tmp = cv.cvtColor(image, cv.COLOR_BGR2RGB)
            pil_list.append(PIL_Image.fromarray(tmp).convert('RGB'))

        i = 0
        while os.path.exists(os.path.join(self.output_path, f'whiteboard_{i}.pdf')):
            i += 1

        pil_list[0].save(os.path.join(self.output_path, f'whiteboard_{i}.pdf'), save_all=True,
                         append_images=pil_list[1:])

    def on_auto_save_btn_click(self):
        self.is_auto_save = not self.is_auto_save

    def auto_save(self, image):
        # threshold the image so the pen strikes will be uniform
        self.auto_save_cache_images.put(image)
        curr_image = cv.cvtColor(image, cv.COLOR_BGR2GRAY)
        _, curr_image = cv.threshold(curr_image, 240, 255, cv.THRESH_BINARY_INV)
        curr_average = np.average(curr_image)
        self.auto_save_cache_averages.put(np.average(curr_average))

        if self.auto_save_cache_images.full():
            prev_image = self.auto_save_cache_images.get()
            prev_average = self.auto_save_cache_averages.get()
            if prev_average/curr_average > 1.5:
                self.saved_image_dict[str(self.image_counter)] = prev_image
                self.bus.add_saved_image(prev_image, self.image_counter)
                self.image_counter += 1

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
