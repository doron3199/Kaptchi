import numpy as np
import cv2
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
from threading import Thread
from scipy.signal import find_peaks
import time

HIGH_VALUE = 10000
ZOOM_VALUE_FULL = 0.5
GENERAL_FPS = 30
NUMBER_OF_PARTS = 50
AUTO_SAVE_QUEUE_SIZE = int(GENERAL_FPS * 3)


class Backend(Widget):
    def __init__(self, **kwargs):
        super(Backend, self).__init__(**kwargs)
        self.play = True
        self.transform = Transform().start()
        self.raw_videos_path = ''
        self.edited_videos_path = ''
        self.output_path = ''
        self.database_init()
        self.bus = None
        self.cap = None
        self.loop_func = None
        self.ports = self.list_ports()
        self.second_in_video = 0
        self.port_num = 0
        self.current_frame = None
        self.is_auto_save = False
        self.auto_save_counter = 0
        self.prev_frame_time = 0
        self.new_frame_time = 0
        self.saved_image_dict = {}
        self.image_counter = 0
        self.auto_save = AutoSave(self.saved_image_dict, self.image_counter, self.bus).start()

    def set(self, bus: Bus):
        self.bus = bus
        self.start()

    def start(self):
        """use to start the camera in the beginning"""
        self.cap = cv2.VideoCapture(self.ports[self.port_num], cv2.CAP_DSHOW)
        # HIGH_VALUE set the highest resolution of the camera, even if it not the actual resolution
        self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, HIGH_VALUE)
        self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, HIGH_VALUE)
        if not self.cap.isOpened():
            logging.error("Cannot open camera")
        self.loop_func = Clock.schedule_interval(self.send_video, 1 / GENERAL_FPS)

    def send_video(self, _):
        """send video to the bus"""
        if self.play:
            # Capture frame-by-frame
            frame = self.get_frame()
            if frame is None:
                self.loop_func.cancel()
                return

            # put the latest image in the queue
            self.transform.put(frame)
            transform_frame = None
            # check if the latest image finished, it may be from previous loop
            if self.transform.more():
                transform_frame = self.transform.read()

            # if remove foreground is on, we don't need to worry about lagging,
            # se we can put images not in 30 fps
            if self.transform.get_variable('is_remove_foreground_on'):
                if transform_frame is not None:
                    self.current_frame = transform_frame
            else:
                # if not, we need to put images at a fast FPS, so we take the original frame and
                # use it. with the fast transform function we can still zoom in and move
                self.current_frame = self.fast_transform(frame, self.transform.get_variable('points_to_cut'),
                                                         self.transform.get_variable('zoom'))
            # just for the first frame
            if self.current_frame is None:
                self.current_frame = frame

            # make sure that auto save is working while sending there the images
            if self.is_auto_save:
                self.auto_save.turn_on()
                self.auto_save.put(self.transform.get_last_removed_foreground())
            else:
                self.auto_save.turn_of()
            self.bus.update_main_image(self.current_frame)
            self.bus.update_video_slider(self.second_in_video)

    def print_fps(self):
        self.new_frame_time = time.time()
        fps = 1 / (self.new_frame_time - self.prev_frame_time)
        self.prev_frame_time = self.new_frame_time
        print(int(fps))

    def fast_transform(self, image, four_points, zoom):
        """this is fast transform function that should not make lagging
        use for when the remove foreground is off so we need flow motion"""
        if zoom != ZOOM_VALUE_FULL:
            image = self.transform.zoom_image(image)
        if len(four_points) == 4:
            image = four_point_transform(image, four_points)
        return image

    def get_frame(self):
        """get frame from camera or video"""
        if isinstance(self.cap, FileVideoStream):
            if self.cap.more():
                self.second_in_video = int(self.cap.stream.get(cv2.CAP_PROP_POS_MSEC) / 1000)
                return self.cap.read()
            else:
                return None
        else:
            ret, frame = self.cap.read()
            if ret:
                return frame
            else:
                return None

    def on_change_zoom_center(self, x, y):
        zoom = self.transform.get_variable('zoom')
        self.transform.set_variable('zoom_center_x', self.transform.get_variable('zoom_center_x') + int(x * zoom * 9))
        self.transform.set_variable('zoom_center_y', self.transform.get_variable('zoom_center_y') + int(-y * zoom * 9))

    def on_change_camera_btn_click(self):
        self.transform.set_variable('final_image', None)
        self.port_num = (self.port_num + 1) % len(self.ports)
        self.cap = cv2.VideoCapture(self.ports[self.port_num], cv2.CAP_DSHOW)
        # HIGH_VALUE set the highest resolution of the camera, even if it not the actual resolution
        self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, HIGH_VALUE)
        self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, HIGH_VALUE)
        self.loop_func.cancel()
        self.loop_func = Clock.schedule_interval(self.send_video, 1 / GENERAL_FPS)

    def on_video_link_btn_click(self, link):
        self.transform.set_variable('final_image', None)
        if 'www.youtube.com' in link:
            yt = YouTube(link)
            video = yt.streams.get_highest_resolution()
            self.cap = FileVideoStream(video.url, queue_size=5).start()
            self.loop_func.cancel()
            fps = video.fps
            self.loop_func = Clock.schedule_interval(self.send_video, 1 / fps)
            video_length_in_seconds = yt.length
            self.bus.set_video_bar(video_length_in_seconds)
        else:
            self.cap = FileVideoStream(link, queue_size=5).start()
            video_length_in_seconds = int(self.cap.stream.get(
                cv2.CAP_PROP_FRAME_COUNT) / self.cap.stream.get(cv2.CAP_PROP_FPS))
            self.bus.set_video_bar(video_length_in_seconds)
            self.loop_func.cancel()
            self.loop_func = Clock.schedule_interval(self.send_video, 1 / GENERAL_FPS)

    def set_video_time(self, value):
        with self.cap.Q.mutex:
            self.cap.Q.queue.clear()
            self.cap.stream.set(cv2.CAP_PROP_POS_MSEC, value * 1000)

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
        self.transform.set_variable('is_whiteboard_filter_on',
                                    not self.transform.get_variable('is_whiteboard_filter_on'))

    def on_remove_foreground_btn_click(self):
        self.transform.set_variable('is_remove_foreground_on',
                                    not self.transform.get_variable('is_remove_foreground_on'))

    def on_zoom_change(self, zoom):
        """we receive the zoom value as percentage, set it as a factor of magnification. multiply by
        0.99 because zoom = 0 will break the program. device by 2 because we do it from the middle"""
        self.transform.set_variable('zoom', (1 - zoom * 0.99) / 2)

    def cut_region(self, cut_region):
        self.transform.set_variable('points_to_cut', np.array(cut_region))

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
            tmp = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
            pil_list.append(PIL_Image.fromarray(tmp).convert('RGB'))

        i = 0
        while os.path.exists(os.path.join(self.output_path, f'whiteboard_{i}.pdf')):
            i += 1

        pil_list[0].save(os.path.join(self.output_path, f'whiteboard_{i}.pdf'), save_all=True,
                         append_images=pil_list[1:])

    def on_auto_save_btn_click(self):
        self.is_auto_save = not self.is_auto_save

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


class Transform:
    def __init__(self, queue_size=1):
        self.stopped = False
        self.zoom = 0.5
        self.zoom_center_x = 0
        self.zoom_center_y = 0
        self.is_whiteboard_filter_on = False
        self.image_processing = WhiteboardFilter()
        self.points_to_cut = []
        self.fps = 0
        self.slider_time = 0
        self.is_remove_foreground_on = False
        self.fgbg = cv2.createBackgroundSubtractorKNN(dist2Threshold=1200, detectShadows=False)
        self.parts = []
        self.final_image = None
        self.last_removed_foreground = None
        self.wighted_average_image = None

        # initialize the queue used to store frames read from
        # the video file
        self.Q_in = Queue(maxsize=queue_size)
        self.Q_out = Queue(maxsize=queue_size)

        # intialize thread
        self.thread = Thread(target=self.update, args=())
        self.thread.daemon = True

    def get_last_removed_foreground(self):
        return self.last_removed_foreground

    def set_variable(self, key, value):
        with self.Q_in.mutex:
            self.Q_in.queue.clear()
            self.__setattr__(key, value)

    def get_variable(self, key):
        with self.Q_in.mutex:
            return self.__getattribute__(key)

    def start(self):
        # start a thread to read frames from the file video stream
        self.thread.start()
        return self

    def put(self, image):
        if not self.Q_in.full():
            self.Q_in.put(image)

    def update(self):
        while True:
            if not self.Q_in.empty():
                image = self.Q_in.get()
                with self.Q_in.mutex:
                    image = self.transform(image)
                self.Q_out.put(image)
            else:
                time.sleep(0.1)

    def read(self):
        # return next frame in the queue
        return self.Q_out.get()

    # Insufficient to have consumer use while(more()) which does
    # not take into account if the producer has reached end of
    # file stream.
    def running(self):
        return self.more() or not self.stopped

    def more(self):
        return self.Q_out.qsize() > 0

    def stop(self):
        # indicate that the thread should be stopped
        self.stopped = True
        # wait until stream resources are released (producer thread might be still grabbing frame)
        self.thread.join()

    def transform(self, image):
        # if frame is read correctly ret is True
        if self.is_remove_foreground_on:
            image = self.remove_foreground(image)
            self.last_removed_foreground = image
        else:
            # then there is still a background picture to use
            self.last_removed_foreground = self.remove_foreground(image)
        if self.zoom != ZOOM_VALUE_FULL:
            image = self.zoom_image(image)
        if self.is_whiteboard_filter_on:
            image = self.image_processing.clean_image(image)
        if len(self.points_to_cut) == 4:
            image = four_point_transform(image, self.points_to_cut)
        return image

    def remove_foreground(self, image):
        blur = cv2.blur(image, (27, 27))
        # normalize the luminance
        v = cv2.cvtColor(blur, cv2.COLOR_BGR2HSV)[:, :, 2]
        v = cv2.equalizeHist(v)
        # apply the background subtractor on this luminance
        fgmask = self.fgbg.apply(v)
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
        return cv2.resize(cropped, (w, h))


class AutoSave:
    def __init__(self, saved_image_dict, image_counter, bus, queue_size=1):
        self.bus = bus
        self.saved_image_dict = saved_image_dict
        self.image_counter = image_counter
        self.auto_save_cache_images = []
        self.auto_save_cache_averages = []
        # initialize the queue used to store frames read from
        # the video file
        self.Q = Queue(maxsize=queue_size)
        self.is_on = False
        self.stopped = False
        # intialize thread
        self.thread = Thread(target=self.update, args=())
        self.thread.daemon = True
        self.max_ratio = 0

    def start(self):
        # start a thread to read frames from the file video stream
        self.thread.start()
        return self

    def turn_on(self):
        self.is_on = True

    def put(self, image):
        if not self.Q.full():
            self.Q.put(image)

    def update(self):
        while True:
            if not self.Q.empty() and self.is_on:
                self.auto_save(self.Q.get())
            else:
                time.sleep(1)

    def turn_of(self):
        self.is_on = False

    def auto_save(self, image):
        # threshold the image so the pen strikes will be uniform
        self.auto_save_cache_images.append(image)
        curr_image = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
        _, curr_image = cv2.threshold(curr_image, 240, 255, cv2.THRESH_BINARY_INV)
        curr_average = np.sum(curr_image)
        self.auto_save_cache_averages.append(np.average(curr_average))

    def find_full_boards(self):

        maxi = find_peaks(self.auto_save_cache_averages, width=5,distance=25)[0]
        full_boards = [self.auto_save_cache_images[x] for x in maxi]



    def send_to_ui(self, image):
        self.saved_image_dict[str(self.image_counter)] = image
        self.bus.add_saved_image(image, self.image_counter)
        self.image_counter += 1
