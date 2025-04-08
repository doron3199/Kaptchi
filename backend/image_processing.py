import cv2 as cv
import numpy as np

NUMBER_OF_PARTS = 15

class ImageProsessing:
    fgbg = cv.createBackgroundSubtractorKNN()
    fgbg.setHistory(300)
    
    def clean_image(self, image: np.ndarray) -> np.ndarray:
        """clean get an image of a whiteboard and returns an improved colored image"""
        #  get an image of the blurred_image whiteboard
        blurred_image = cv.medianBlur(image, 7)
        blurred_image = cv.GaussianBlur(blurred_image, (3, 3), 0)


        # you should read the microsoft research for this lines
        normalized_image = np.minimum(image / blurred_image, 1)
        # enhanced_image = 0.5 - 0.5 * np.cos(np.power(normalized_image, 2.5) * np.pi)
        enhanced_image = 0.5 - 0.5 * np.cos(np.power(normalized_image, 5) * np.pi)

        # transform the image back to 0 to 255, it transform to 0-1 when it divided.
        result_image = (enhanced_image * 255).astype(np.uint8)

        return result_image
    



    def remove_foreground(
        self, 
        image: np.ndarray,
        final_image: np.ndarray = None 
    ) -> np.ndarray:
        """
        Removes foreground elements from an image using a background subtractor.

        Args:
            final_image (np.ndarray): The accumulated background image.
            image (np.ndarray): The current frame to process.

        Returns:
            np.ndarray: The updated background image with foreground removed.

        Raises:
            ValueError: If final_image and image have different dimensions.
        """
        if final_image is None:
            final_image = image.copy()
        if final_image.shape != image.shape:
            final_image = cv.resize(final_image, (image.shape[1], image.shape[0]))
        
        scale = 0.1
        small_frame = cv.resize(image, (0, 0), fx=scale, fy=scale)

        fgmask = self.fgbg.apply(small_frame)
        _, w = image.shape[0:2]
        dist = np.linspace(0, w, NUMBER_OF_PARTS, dtype=int)
        _, mask_w = fgmask.shape[0:2]
        fgmask_dist = np.linspace(0, mask_w, NUMBER_OF_PARTS, dtype=int)

        fgmask_sums = np.add.reduceat(fgmask, fgmask_dist[:-1], axis=1)
        is_static = fgmask_sums.sum(axis=0) == 0

        # # Create a mask for the entire image based on is_static
        mask = np.zeros_like(final_image, dtype=bool)

        # the mask is static only if the agecent part is static and the next part is static
        # it reduces the noise in the image
        for i, (start, end) in enumerate(zip(dist[:-1], dist[1:])):
            if i == 0:
                mask[:, start:end] = is_static[i] and is_static[i + 1]
            elif i == NUMBER_OF_PARTS - 2:
                mask[:, start:end] = is_static[i] and is_static[i - 1]
            else:
                mask[:, start:end] = is_static[i] and is_static[i + 1] and is_static[i - 1]

        # Use the mask to update final_image in a single operation
        final_image = np.where(mask, image, final_image)
        return final_image
    

