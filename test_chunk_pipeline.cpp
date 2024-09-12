#include "test_chunk_pipeline.h"
#include <syslog.h>
#include <regex>
#include <system_error>

static const char *launch_string  =
        "videotestsrc \
            name=%_chunk_video \
            ! video/x-raw,width=1920,height=1080,format=I420 \
            ! clockoverlay \
            ! x264enc tune=zerolatency bitrate=8000 \
        ! tee name=vtee \
          vtee. ! queue ! avdec_h264 ! videoconvert ! videoscale ! autovideosink  \
          vtee. ! queue name=%_chunk_vrecq ! mp4mux name=%_chunk_mux ! filesink async=false name=%_chunk_filesink";

std::string TestChunkPipeline::GetSource() const {return "test_chunk_src";}
std::string TestChunkPipeline::GetSink() const {return "test_filesink";}
std::string TestChunkPipeline::GetLaunchString() const {return _LaunchString;}

void TestChunkPipeline::_SetNames(const std::string &name)
{
    std::regex re("%");
    std::string replace = std::regex_replace(_LaunchString, re, name);
    _LaunchString = replace;
    _FileSinkName = name + "_chunk_filesink";
    _MuxName = name + "_chunk_mux";
    _QueueName = name + "_chunk_vrecq";
}


/* TODO: change location of filesink. For now, this is fine. */
static void
app_update_filesink_location (ChunkApp * app)
{
  gchar *fn;

  fn = g_strdup_printf ("%s-%03d.mp4", app->prefix_location.c_str(), app->chunk_count++);
  g_print ("Setting filesink location to '%s'\n", fn);
  g_object_set (app->filesink, "location", fn, NULL);
  g_free (fn);
}

static GstPadProbeReturn
probe_drop_one_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  ChunkApp *app = (ChunkApp*) user_data;
  GstBuffer *buf = (GstBuffer*) info->data;
  (void) pad;

  if (app->buffer_count++ == 0) {
    g_print ("Drop one buffer with ts %" GST_TIME_FORMAT "\n",
        GST_TIME_ARGS (GST_BUFFER_PTS (info->data)));
    return GST_PAD_PROBE_DROP;
  } else {
    gboolean is_keyframe;

    is_keyframe = !GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT);
    g_print ("Buffer with ts %" GST_TIME_FORMAT " (keyframe=%d)\n",
        GST_TIME_ARGS (GST_BUFFER_PTS (buf)), is_keyframe);

    if (is_keyframe) {
      g_print ("Letting buffer through and removing drop probe\n");
      return GST_PAD_PROBE_REMOVE;
    } else {
      g_print ("Dropping buffer, wait for a keyframe.\n");
      return GST_PAD_PROBE_DROP;
    }
  }
}

static GstPadProbeReturn
block_probe_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  g_print ("pad %s:%s blocked!\n", GST_DEBUG_PAD_NAME (pad));
  (void) user_data;
  g_assert ((info->type & GST_PAD_PROBE_TYPE_BUFFER) ==
      GST_PAD_PROBE_TYPE_BUFFER);
  /* FIXME: this doesn't work: gst_buffer_replace ((GstBuffer **) &info->data, NULL); */
  return GST_PAD_PROBE_OK;
}

static gpointer
push_eos_thread (gpointer user_data)
{
  ChunkApp *app = (ChunkApp*) user_data;
  GstPad *peer;

  peer = gst_pad_get_peer (app->vrecq_src);
  g_print ("pushing EOS event on pad %s:%s\n", GST_DEBUG_PAD_NAME (peer));

  /* tell pipeline to forward EOS message from filesink immediately and not
   * hold it back until it also got an EOS message from the video sink */
  g_object_set (app->pipeline, "message-forward", TRUE, NULL);

  gst_pad_send_event (peer, gst_event_new_eos ());
  gst_object_unref (peer);

  return NULL;
}

static gboolean
stop_recording_cb (gpointer user_data)
{
  ChunkApp *app = (ChunkApp*) user_data; //this needs fixing...

  g_print ("stop recording\n");

  app->vrecq_src_probe_id = gst_pad_add_probe (app->vrecq_src,
      (GstPadProbeType) (GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_BUFFER), block_probe_cb,
      NULL, NULL);

  g_thread_new ("eos-push-thread", push_eos_thread, app);

  return FALSE;                 /* don't call us again */
}

static gboolean
start_recording_cb (gpointer user_data)
{
  ChunkApp *app = (ChunkApp*) user_data;

  g_print ("unblocking pad to start recording\n");

  /* need to hook up another probe to drop the initial old buffer stuck
   * in the blocking pad probe */
  app->buffer_count = 0;
  gst_pad_add_probe (app->vrecq_src,
      GST_PAD_PROBE_TYPE_BUFFER, probe_drop_one_cb, app, NULL);

  /* now remove the blocking probe to unblock the pad */
  gst_pad_remove_probe (app->vrecq_src, app->vrecq_src_probe_id);
  app->vrecq_src_probe_id = 0;

  g_timeout_add_seconds (5, stop_recording_cb, app);

  return FALSE;                 /* don't call us again */
}

static gboolean
bus_cb (GstBus * bus, GstMessage * msg, gpointer user_data)
{
  ChunkApp *app = (ChunkApp*) user_data;

  (void)bus;

  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_ERROR:
      g_print ("Error!\n");
      g_main_loop_quit (app->loop);
      return FALSE;
    case GST_MESSAGE_ELEMENT:{
      const GstStructure *s = gst_message_get_structure (msg);

      if (gst_structure_has_name (s, "GstBinForwarded")) {
        GstMessage *forward_msg = NULL;

        gst_structure_get (s, "message", GST_TYPE_MESSAGE, &forward_msg, NULL);
        if (GST_MESSAGE_TYPE (forward_msg) == GST_MESSAGE_EOS) {
          g_print ("EOS from element %s\n",
              GST_OBJECT_NAME (GST_MESSAGE_SRC (forward_msg)));
          gst_element_set_state (app->filesink, GST_STATE_NULL);
          gst_element_set_state (app->muxer, GST_STATE_NULL);
          app_update_filesink_location (app);
          gst_element_set_state (app->filesink, GST_STATE_PLAYING);
          gst_element_set_state (app->muxer, GST_STATE_PLAYING);
          /* do another recording in 10 secs time */
          g_timeout_add_seconds (10, start_recording_cb, app);
        }
        gst_message_unref (forward_msg);
      }
      break;
    }
    default:
      break;
  }

  return TRUE;
}

TestChunkPipeline::TestChunkPipeline(const std::string &name):
    _LaunchString(launch_string)
{

    gst_init (NULL, NULL);
    _SetNames(name);
    _app.pipeline =
      gst_parse_launch (_LaunchString.c_str(),
      NULL);
}



void
TestChunkPipeline::Launch() {
    g_print("%s\n", _LaunchString.c_str());
    g_print("Looking for %s\n", _QueueName.c_str());
    _app.vrecq = gst_bin_get_by_name (GST_BIN (_app.pipeline), _QueueName.c_str());
    g_object_set (_app.vrecq, "max-size-time", (guint64) 5 * GST_SECOND,
        "max-size-bytes", 0, "max-size-buffers", 0, "leaky", 2, NULL);

    _app.vrecq_src = gst_element_get_static_pad (_app.vrecq, "src");
    _app.vrecq_src_probe_id = gst_pad_add_probe (_app.vrecq_src,
        (GstPadProbeType) (GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_BUFFER), block_probe_cb,
        NULL, NULL);

    _app.chunk_count = 0;
    _app.filesink = gst_bin_get_by_name (GST_BIN (_app.pipeline), _FileSinkName.c_str());
    _app.prefix_location = "/tmp/recording";
    app_update_filesink_location (&_app);
    _app.muxer = gst_bin_get_by_name (GST_BIN (_app.pipeline), _MuxName.c_str());

    gst_element_set_state (_app.pipeline, GST_STATE_PLAYING);

    _app.loop = g_main_loop_new (NULL, FALSE);
    gst_bus_add_watch (GST_ELEMENT_BUS (_app.pipeline), bus_cb, &_app);

    g_timeout_add_seconds (10, start_recording_cb, &_app);
    g_main_loop_run (_app.loop);

    gst_element_set_state (_app.pipeline, GST_STATE_NULL);
    gst_object_unref (_app.pipeline);
}

/*TODO: regex filesink and chunk_mux */
int
main (int argc, char **argv)
{
    TestChunkPipeline* chunky = new TestChunkPipeline("TEST");
    chunky->Launch();

}
