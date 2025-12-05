import 'dart:io';
import 'package:flutter/material.dart';
import 'package:camera/camera.dart' as c;
import 'package:flutter_webrtc/flutter_webrtc.dart';
import 'package:haishin_kit/stream_view_texture.dart';
import 'package:haishin_kit/rtmp_stream.dart';
import '../services/rtmp_service.dart';
import '../models/stream_protocol.dart';
import '../widgets/native_texture_view.dart';

class CameraStreamView extends StatelessWidget {
  final StreamProtocol selectedProtocol;
  final bool supportsMobileRtmp;
  final c.CameraController? controller;
  final RTCVideoRenderer localRenderer;
  final String? connectionUrl;

  const CameraStreamView({
    super.key,
    required this.selectedProtocol,
    required this.supportsMobileRtmp,
    required this.controller,
    required this.localRenderer,
    this.connectionUrl,
  });

  @override
  Widget build(BuildContext context) {
    debugPrint(
      'Building Stream Widget. Protocol: $selectedProtocol, Windows: ${Platform.isWindows}',
    );
    if (selectedProtocol == StreamProtocol.rtmp && supportsMobileRtmp) {
      // On Android, if we are "transmitting" (simulated), show the local camera
      // If we are actually streaming via HaishinKit, we should show its preview
      return ValueListenableBuilder<RtmpStream?>(
        valueListenable: RtmpService.instance.streamNotifier,
        builder: (context, stream, child) {
          if (stream != null) {
            return Stack(
              children: [
                // Always show the texture if the stream exists
                StreamViewTexture(stream),

                // Show loading overlay if not yet connected/publishing
                ValueListenableBuilder<bool>(
                  valueListenable: RtmpService.instance.isStreamingNotifier,
                  builder: (context, isStreaming, child) {
                    if (!isStreaming) {
                      return const Center(
                        child: Column(
                          mainAxisAlignment: MainAxisAlignment.center,
                          children: [
                            CircularProgressIndicator(color: Colors.white),
                            SizedBox(height: 16),
                            Text(
                              'Connecting to RTMP Server...',
                              style: TextStyle(color: Colors.white),
                            ),
                          ],
                        ),
                      );
                    }
                    return const SizedBox.shrink();
                  },
                ),
              ],
            );
          }

          // If we are supposed to be streaming but RtmpService isn't ready yet, show loading
          if (connectionUrl != null && connectionUrl!.startsWith('rtmp://')) {
            return const Center(
              child: Column(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  CircularProgressIndicator(color: Colors.white),
                  SizedBox(height: 16),
                  Text(
                    'Initializing Service...',
                    style: TextStyle(color: Colors.white),
                  ),
                ],
              ),
            );
          }

          if (controller != null && controller!.value.isInitialized) {
            return Center(
              child: AspectRatio(
                aspectRatio: controller!.value.aspectRatio,
                child: c.CameraPreview(controller!),
              ),
            );
          }
          return const Center(
            child: Text(
              'Initializing Camera...',
              style: TextStyle(color: Colors.white),
            ),
          );
        },
      );
    }

    if (Platform.isWindows && selectedProtocol == StreamProtocol.rtmp) {
      debugPrint('Using NativeTextureView');
      return const NativeTextureView();
    }

    if (selectedProtocol == StreamProtocol.webrtc) {
      return RTCVideoView(
        localRenderer,
        objectFit: RTCVideoViewObjectFit.RTCVideoViewObjectFitContain,
        mirror: false, // Do not mirror the back camera
      );
    }

    return const Center(child: CircularProgressIndicator());
  }
}
