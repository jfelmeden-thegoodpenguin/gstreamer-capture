#ifndef _TEST_CHUNK_PIPELINE_
#define _TEST_CHUNK_PIPELINE_

#include <gst/gst.h>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>

class ChunkApp
{
    public:
        GstElement *pipeline;
        GstElement *vrecq;
        GstElement *filesink;
        GstElement *muxer;
        GMainLoop *loop;
        GstPad *vrecq_src;
        gulong vrecq_src_probe_id;
        guint buffer_count;
        guint chunk_count;
        std::string prefix_location;
};

class TestChunkPipeline
{
    public:
        TestChunkPipeline(const std::string &name);
        void Launch();

        std::string GetSink() const;
        std::string GetSource() const;
        std::string GetLaunchString() const;

        // /**
        //  * @brief prepare chunk pipeline for recording.
        //  * Call before changing pipeline to playing
        //  * @param locationPrefix: file location to prefix
        //  * (i.e., if prefix is /home/root/video.mp4, records chunk x to
        //  * /home/root/video-chunk-x.mp4)
        //  */
        // virtual void OpenChunk(std::string locationPrefix);
        // /**
        //  * @brief finalise chunk pipeline. Call before setting pipeline to
        //  * null
        //  *
        //  * @param eos: If true, send an EOS on the pipeline to close the mp4
        //  * chunk. If the pipeline is in an erroneous state, it is
        //  * recommended to skip this to avoid blocking here.
        //  * @return 0 on success
        //  */
        // virtual int CloseChunk(bool eos=true);

        // /* start a new chunk */
        // virtual int StartChunk();

        // /* stop the current chunk */
        // virtual int StopChunk();
        std::string _PrefixString;
    private:
        std::string _PipelineName;
        std::string _LaunchString;
        std::string _FileSinkName;
        std::string _MuxName;
        std::string _QueueName;

        std::string _FileLocation;

        ChunkApp _app;

        void _SetNames(const std::string &name);

        void SetLocation(const std::string &location="/home/root/video-chunk-%.mp4");
};
#endif
