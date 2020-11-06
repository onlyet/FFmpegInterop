//*****************************************************************************
//
//	Copyright 2015 Microsoft Corporation
//
//	Licensed under the Apache License, Version 2.0 (the "License");
//	you may not use this file except in compliance with the License.
//	You may obtain a copy of the License at
//
//	http ://www.apache.org/licenses/LICENSE-2.0
//
//	Unless required by applicable law or agreed to in writing, software
//	distributed under the License is distributed on an "AS IS" BASIS,
//	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//	See the License for the specific language governing permissions and
//	limitations under the License.
//
//*****************************************************************************

#include "pch.h"
#include "FFmpegInteropMSS.h"
#include "FFmpegInteropMSS.g.cpp"
#include "StreamFactory.h"
#include "SampleProvider.h"
#include "Metadata.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::Foundation::Metadata;
using namespace winrt::Windows::Media::Core;
using namespace winrt::Windows::Storage::Streams;
using namespace std;

namespace
{
	using namespace winrt::FFmpegInterop::implementation;

	// Function to read from file stream. Credit to Philipp Sch http://www.codeproject.com/Tips/489450/Creating-Custom-FFmpeg-IO-Context
	int FileStreamRead(_In_ void* ptr, _In_ uint8_t* buf, _In_ int bufSize) noexcept
	{
		IStream* fileStream{ reinterpret_cast<IStream*>(ptr) };
		ULONG bytesRead{ 0 };

		RETURN_AVERROR_IF(AVERROR_EXTERNAL, FAILED(fileStream->Read(buf, bufSize, &bytesRead)));

		// Assume we've reached EOF if we didn't read any bytes
		RETURN_AVERROR_IF(AVERROR_EOF, bytesRead == 0);

		return bytesRead;
	}

	// Function to seek in file stream. Credit to Philipp Sch http://www.codeproject.com/Tips/489450/Creating-Custom-FFmpeg-IO-Context
	int64_t FileStreamSeek(_In_ void* ptr, _In_ int64_t pos, _In_ int whence) noexcept
	{
		IStream* fileStream{ reinterpret_cast<IStream*>(ptr) };
		LARGE_INTEGER in{ 0 };
		in.QuadPart = pos;
		ULARGE_INTEGER out{ 0 };

		RETURN_AVERROR_IF(AVERROR_EXTERNAL, FAILED(fileStream->Seek(in, whence, &out)));

		return out.QuadPart;
	}
}

namespace winrt::FFmpegInterop::implementation
{
	FFmpegInterop::FFmpegInteropMSS FFmpegInteropMSS::CreateFromStream(_In_ const IRandomAccessStream& fileStream, _In_opt_ const MediaStreamSource& mss, _In_opt_ const FFmpegInterop::FFmpegInteropMSSConfig& config)
	{
		auto logger{ CreateFromStreamActivity::Start() };

		FFmpegInterop::FFmpegInteropMSS ffmpegInteropMSS{ make<FFmpegInteropMSS>(fileStream, mss, config) };

		logger.Stop();

		return ffmpegInteropMSS;
	}

	FFmpegInterop::FFmpegInteropMSS FFmpegInteropMSS::CreateFromUri(_In_ const hstring& uri, _In_opt_ const MediaStreamSource& mss, _In_opt_ const FFmpegInterop::FFmpegInteropMSSConfig& config)
	{
		auto logger{ CreateFromUriActivity::Start() };

		FFmpegInterop::FFmpegInteropMSS ffmpegInteropMSS{ make<FFmpegInteropMSS>(uri, mss, config) };

		logger.Stop();

		return ffmpegInteropMSS;
	}

	FFmpegInteropMSS::FFmpegInteropMSS(_In_opt_ const MediaStreamSource& mss) :
		m_mss(mss),
		m_formatContext(avformat_alloc_context()),
		m_reader(m_formatContext.get(), m_streamIdMap)
	{
		THROW_IF_NULL_ALLOC(m_formatContext);

		// Activate a new MSS instance if one was not provided
		if (m_mss == nullptr)
		{
			IActivationFactory mssFactory{ get_activation_factory<MediaStreamSource>() };
			m_mss = mssFactory.ActivateInstance<MediaStreamSource>();
		}
	}

	FFmpegInteropMSS::FFmpegInteropMSS(_In_ const IRandomAccessStream& fileStream, _In_opt_ const MediaStreamSource& mss, _In_opt_ const FFmpegInterop::FFmpegInteropMSSConfig& config) :
		FFmpegInteropMSS(mss)
	{
		try
		{
			OpenFile(fileStream, config);
			InitFFmpegContext(config);
		}
		catch (...)
		{
			if (m_mss != nullptr)
			{
				// Notify the MSS that an error occurred
				m_mss.NotifyError(MediaStreamSourceErrorStatus::UnsupportedMediaFormat);
			}

			throw;
		}
	}

	FFmpegInteropMSS::FFmpegInteropMSS(_In_ const hstring& uri, _In_opt_ const MediaStreamSource& mss, _In_opt_ const FFmpegInterop::FFmpegInteropMSSConfig& config) :
		FFmpegInteropMSS(mss)
	{
		try
		{
			wstring_convert<codecvt_utf8<wchar_t>> conv;
			string uriA{ conv.to_bytes(uri.c_str()) };

			OpenFile(uriA.c_str(), config);
			InitFFmpegContext(config);
		}
		catch (...)
		{
			if (m_mss != nullptr)
			{
				// Notify the MSS that an error occurred
				m_mss.NotifyError(MediaStreamSourceErrorStatus::UnsupportedMediaFormat);
			}

			throw;
		}
	}

	void FFmpegInteropMSS::OpenFile(_In_ const IRandomAccessStream& fileStream, _In_opt_ const FFmpegInterop::FFmpegInteropMSSConfig& config)
	{
		// Convert async IRandomAccessStream to sync IStream
		THROW_HR_IF_NULL(E_INVALIDARG, fileStream);
		THROW_IF_FAILED(CreateStreamOverRandomAccessStream(static_cast<::IUnknown*>(get_abi(fileStream)), __uuidof(m_fileStream), m_fileStream.put_void()));

		// Setup FFmpeg custom IO to access file as stream. This is necessary when accessing any file outside of app installation directory and appdata folder.
		// Credit to Philipp Sch http://www.codeproject.com/Tips/489450/Creating-Custom-FFmpeg-IO-Context
		constexpr int c_ioBufferSize = 16 * 1024;
		AVBlob_ptr ioBuffer{ av_malloc(c_ioBufferSize) };
		THROW_IF_NULL_ALLOC(ioBuffer);

		m_ioContext.reset(avio_alloc_context(reinterpret_cast<unsigned char*>(ioBuffer.get()), c_ioBufferSize, 0, m_fileStream.get(), FileStreamRead, nullptr, FileStreamSeek));
		THROW_IF_NULL_ALLOC(m_ioContext);
		ioBuffer.release(); // The IO context has taken ownership of the buffer

		m_formatContext->pb = m_ioContext.get();

		OpenFile("", config);
	}

	void FFmpegInteropMSS::OpenFile(_In_z_ const char* uri, _In_opt_ const FFmpegInterop::FFmpegInteropMSSConfig& config)
	{
		// Parse the FFmpeg options in config if present
		AVDictionary_ptr options;
		if (config != nullptr)
		{
			wstring_convert<codecvt_utf8<wchar_t>> conv;
			StringMap ffmpegOptions{ config.FFmpegOptions() };
			for (const auto& iter : ffmpegOptions)
			{
				hstring key{ iter.Key() };
				string keyA{ conv.to_bytes(key.c_str()) };

				hstring value{ iter.Value() };
				string valueA{ conv.to_bytes(value.c_str()) };

				AVDictionary* optionsRaw{ options.release() };
				int result{ av_dict_set(&optionsRaw, keyA.c_str(), valueA.c_str(), 0) };
				options.reset(exchange(optionsRaw, nullptr));
				THROW_HR_IF_FFMPEG_FAILED(result);
			}
		}

		// Open the format context for the stream
		AVFormatContext* formatContextRaw{ m_formatContext.release() };
		AVDictionary* optionsRaw{ options.release() };
		int result{ avformat_open_input(&formatContextRaw, uri, nullptr, &optionsRaw) }; // The format context is freed on failure
		options.reset(exchange(optionsRaw, nullptr));
		THROW_HR_IF_FFMPEG_FAILED(result);
		m_formatContext.reset(exchange(formatContextRaw, nullptr));
	}

	void FFmpegInteropMSS::InitFFmpegContext(_In_opt_ const FFmpegInterop::FFmpegInteropMSSConfig& config)
	{
		THROW_HR_IF_FFMPEG_FAILED(avformat_find_stream_info(m_formatContext.get(), nullptr));

		int preferredAudioStreamId{ av_find_best_stream(m_formatContext.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) };
		int preferredVideoStreamId{ av_find_best_stream(m_formatContext.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0) };
		bool hasAudio{ false };
		bool hasVideo{ false };
		vector<IMediaStreamDescriptor> pendingAudioStreamDescriptors;
		vector<IMediaStreamDescriptor> pendingVideoStreamDescriptors;

		for (unsigned int i{ 0 }; i < m_formatContext->nb_streams; i++)
		{
			AVStream* stream{ m_formatContext->streams[i] };

			// Discard all samples for this stream until it is selected
			stream->discard = AVDISCARD_ALL;

			// Create the sample provider and stream descriptor
			unique_ptr<SampleProvider> sampleProvider;
			IMediaStreamDescriptor streamDescriptor{ nullptr };

			switch (stream->codecpar->codec_type)
			{
			case AVMEDIA_TYPE_AUDIO:
				tie(sampleProvider, streamDescriptor) = StreamFactory::CreateAudioStream(m_formatContext.get(), stream, m_reader, config);

				if (hasAudio || preferredAudioStreamId == i || preferredAudioStreamId < 0)
				{
					// Add the stream to the MSS
					m_mss.AddStreamDescriptor(streamDescriptor);

					// Check if this is the first audio stream added to the MSS
					if (!hasAudio)
					{
						hasAudio = true;
						sampleProvider->Select(); // The first audio stream is selected by default

						// Add any audio streams we already enumerated
						for (auto& audioStreamDescriptor : pendingAudioStreamDescriptors)
						{
							m_mss.AddStreamDescriptor(move(audioStreamDescriptor));
						}
						pendingAudioStreamDescriptors.clear();
					}
				}
				else
				{
					// We'll add this stream to the MSS after the preferred audio stream has been enumerated
					pendingAudioStreamDescriptors.push_back(streamDescriptor);
				}

				break;

			case AVMEDIA_TYPE_VIDEO:
				// FFmpeg identifies album/cover art from a music file as a video stream
				if (stream->disposition == AV_DISPOSITION_ATTACHED_PIC)
				{
					SetMSSThumbnail(m_mss, stream);
					continue;
				}

				tie(sampleProvider, streamDescriptor) = StreamFactory::CreateVideoStream(m_formatContext.get(), stream, m_reader, config);

				if (hasVideo || preferredVideoStreamId == i || preferredVideoStreamId < 0)
				{
					// Add the stream to the MSS
					m_mss.AddStreamDescriptor(streamDescriptor);

					// Check if this is the first video stream added to the MSS
					if (!hasVideo)
					{
						hasVideo = true;
						sampleProvider->Select(); // The first video stream is selected by default

						// Add any video streams we already enumerated
						for (auto& videoStreamDescriptor : pendingVideoStreamDescriptors)
						{
							m_mss.AddStreamDescriptor(move(videoStreamDescriptor));
						}
						pendingVideoStreamDescriptors.clear();
					}
				}
				else
				{
					// We'll add this stream to the MSS after the preferred video stream has been enumerated
					pendingVideoStreamDescriptors.push_back(streamDescriptor);
				}

				break;

			case AVMEDIA_TYPE_SUBTITLE:
				// Subtitle streams use TimedMetadataStreamDescriptor which was added in 17134. Check if this type is present.
				// Note: MSS didn't expose subtitle streams in media engine scenarios until 19041.
				if (!ApiInformation::IsTypePresent(L"Windows.Media.Core.TimedMetadataStreamDescriptor"))
				{
					TraceLoggingWrite(g_FFmpegInteropProvider, "NoSubtitleSupport", TraceLoggingLevel(TRACE_LEVEL_VERBOSE), TraceLoggingPointer(this, "this"),
						TraceLoggingValue(stream->index, "StreamId"),
						TraceLoggingInt32(stream->codecpar->codec_id, "AVCodecID"));
					continue;
				}

				try
				{
					tie(sampleProvider, streamDescriptor) = StreamFactory::CreateSubtitleStream(m_formatContext.get(), stream, m_reader);
				}
				catch (...)
				{
					// Unsupported subtitle stream. Just ignore.
					TraceLoggingWrite(g_FFmpegInteropProvider, "UnsupportedSubtitleStream", TraceLoggingLevel(TRACE_LEVEL_VERBOSE), TraceLoggingPointer(this, "this"),
						TraceLoggingValue(stream->index, "StreamId"),
						TraceLoggingInt32(stream->codecpar->codec_id, "AVCodecID"));
					continue;
				}

				// Add the stream to the MSS
				m_mss.AddStreamDescriptor(streamDescriptor);

				break;

			default:
				// Ignore this stream
				TraceLoggingWrite(g_FFmpegInteropProvider, "UnsupportedStream", TraceLoggingLevel(TRACE_LEVEL_VERBOSE), TraceLoggingPointer(this, "this"),
					TraceLoggingValue(stream->index, "StreamId"),
					TraceLoggingInt32(stream->codecpar->codec_type, "AVMediaType"),
					TraceLoggingInt32(stream->codecpar->codec_id, "AVCodecID"));
				continue;
			}

			// Add the stream to our maps
			m_streamIdMap[i] = sampleProvider.get();
			m_streamDescriptorMap[move(streamDescriptor)] = move(sampleProvider);
		}

		WINRT_ASSERT(pendingAudioStreamDescriptors.empty());
		WINRT_ASSERT(pendingVideoStreamDescriptors.empty());

		if (m_formatContext->duration > 0)
		{
			// Set the duration
			m_mss.Duration(TimeSpan{ ConvertFromAVTime(m_formatContext->duration, av_get_time_base_q(), HNS_PER_SEC) });
			m_mss.CanSeek(true);
		}
		else
		{
			// Set buffer time to 0 for realtime streaming to reduce latency
			m_mss.BufferTime(TimeSpan{ 0 });
		}

		// Populate metadata
		if (m_formatContext->metadata != nullptr)
		{
			PopulateMSSMetadata(m_mss, m_formatContext->metadata);
		}

		// Register event handlers. The delegates hold strong references to tie the lifetime of this object to the MSS.
		m_startingEventToken = m_mss.Starting({ get_strong(), &FFmpegInteropMSS::OnStarting });
		m_sampleRequestedEventToken = m_mss.SampleRequested({ get_strong(), &FFmpegInteropMSS::OnSampleRequested });
		m_switchStreamsRequestedEventToken = m_mss.SwitchStreamsRequested({ get_strong(), &FFmpegInteropMSS::OnSwitchStreamsRequested });
		m_closedEventToken = m_mss.Closed({ get_strong(), &FFmpegInteropMSS::OnClosed });
	}

	void FFmpegInteropMSS::OnStarting(_In_ const MediaStreamSource&, _In_ const MediaStreamSourceStartingEventArgs& args)
	{
		auto logger{ OnStartingActivity::Start() };

		const MediaStreamSourceStartingRequest request{ args.Request() };
		const IReference<TimeSpan> startPosition{ request.StartPosition() };

		// Check if the start position is null. A null start position indicates we're resuming playback from the current position.
		if (startPosition != nullptr)
		{
			lock_guard<mutex> lock{ m_lock };

			const TimeSpan hnsSeekTime{ startPosition.Value() };
			TraceLoggingWrite(g_FFmpegInteropProvider, "Seek", TraceLoggingLevel(TRACE_LEVEL_VERBOSE), TraceLoggingPointer(this, "this"),
				TraceLoggingValue(hnsSeekTime.count(), "SeekTimeHNS"));
			
			try
			{
				// Convert the seek time from HNS to AV_TIME_BASE
				int64_t avSeekTime{ ConvertToAVTime(hnsSeekTime.count(), HNS_PER_SEC, av_get_time_base_q()) };
				THROW_HR_IF(MF_E_INVALID_TIMESTAMP, avSeekTime > m_formatContext->duration);

				if (m_formatContext->start_time != AV_NOPTS_VALUE)
				{
					// Adjust the seek time by the start time offset
					avSeekTime += m_formatContext->start_time;
				}

				THROW_HR_IF_FFMPEG_FAILED(avformat_seek_file(m_formatContext.get(), -1, numeric_limits<int64_t>::min(), avSeekTime, avSeekTime, 0));

				for (auto& [streamId, stream] : m_streamIdMap)
				{
					stream->OnSeek(hnsSeekTime.count());
				}

				request.SetActualStartPosition(hnsSeekTime);

				logger.Stop();
			}
			catch (...)
			{
				// Notify the MSS that an error occurred
				m_mss.NotifyError(MediaStreamSourceErrorStatus::Other);
			}
		}
		else
		{
			TraceLoggingWrite(g_FFmpegInteropProvider, "Resume", TraceLoggingLevel(TRACE_LEVEL_VERBOSE), TraceLoggingPointer(this, "this"));
			logger.Stop();
		}
	}

	void FFmpegInteropMSS::OnSampleRequested(_In_ const MediaStreamSource&, _In_ const MediaStreamSourceSampleRequestedEventArgs& args)
	{
		auto logger{ OnSampleRequestedActivity::Start() };

		const MediaStreamSourceSampleRequest request{ args.Request() };

		lock_guard<mutex> lock{ m_lock };

		try
		{
			// Get the next sample for the stream
			m_streamDescriptorMap.at(request.StreamDescriptor())->GetSample(request);

			logger.Stop();
		}
		catch (...)
		{
			const hresult hr{ to_hresult() };
			if (hr == MF_E_END_OF_STREAM)
			{
				// Notify all streams we're at EOF
				for (auto& [streamId, sampleProvider] : m_streamIdMap)
				{
					sampleProvider->NotifyEOF();
				}

				logger.Stop(); // This is an expected error. No need to log it.
			}
			else
			{
				// Notify the MSS that an error occurred
				m_mss.NotifyError(MediaStreamSourceErrorStatus::Other);
			}
		}
	}

	void FFmpegInteropMSS::OnSwitchStreamsRequested(_In_ const MediaStreamSource&, _In_ const MediaStreamSourceSwitchStreamsRequestedEventArgs& args)
	{
		auto logger{ OnSwitchStreamsRequestedActivity::Start() };

		const MediaStreamSourceSwitchStreamsRequest request{ args.Request() };
		const IMediaStreamDescriptor oldStreamDescriptor{ request.OldStreamDescriptor() };
		const IMediaStreamDescriptor newStreamDescriptor{ request.NewStreamDescriptor() };

		// The old/new stream descriptors should not be the same and at least one should be valid.
		WINRT_ASSERT(oldStreamDescriptor != newStreamDescriptor);

		lock_guard<mutex> lock{ m_lock };

		try
		{
			if (oldStreamDescriptor != nullptr)
			{
				m_streamDescriptorMap.at(oldStreamDescriptor)->Deselect();
			}

			if (newStreamDescriptor != nullptr)
			{
				m_streamDescriptorMap.at(newStreamDescriptor)->Select();
			}

			logger.Stop();
		}
		catch (...)
		{
			WINRT_ASSERT(false);

			// Notify the MSS that an error occurred
			m_mss.NotifyError(MediaStreamSourceErrorStatus::Other);
		}
	}

	void FFmpegInteropMSS::OnClosed(_In_ const MediaStreamSource&, _In_ const MediaStreamSourceClosedEventArgs& args)
	{
		auto logger{ OnClosedActivity::Start() };

		lock_guard<mutex> lock{ m_lock };

		ShutdownInternal();

		logger.Stop();
	}

	STDMETHODIMP FFmpegInteropMSS::GetShutdownStatus(_Out_ MFSHUTDOWN_STATUS* pStatus) noexcept
	{
		auto logger{ GetShutdownStatusActivity::Start() };

		RETURN_HR_IF_NULL(E_POINTER, pStatus);

		lock_guard<mutex> lock{ m_lock };

		RETURN_HR_IF(MF_E_INVALIDREQUEST, m_mss != nullptr);
		*pStatus = MFSHUTDOWN_COMPLETED;

		logger.Stop();
		return S_OK;
	}

	STDMETHODIMP FFmpegInteropMSS::Shutdown() noexcept
	try
	{
		auto logger{ ShutdownActivity::Start() };

		lock_guard<mutex> lock{ m_lock };

		ShutdownInternal();

		logger.Stop();
		return S_OK;
	}
	CATCH_RETURN();

	void FFmpegInteropMSS::ShutdownInternal()
	{
		if (m_mss != nullptr)
		{
			// Release the file stream
			// This is critically important to do for the media source app service scenario! The remote app process may be suspended anytime after 
			// this Closed event is processed. If we don't release the file stream now, then we'll effectively leak the file handle which could 
			// cause file related issues until the remote app process is terminated.
			m_fileStream = nullptr;

			// Unregister event handlers
			// This is critically important to do for the media source app service scenario! If we don't unregister these event handlers, then
			// they'll be released when the MSS is destroyed. That kicks off a race condition between COM releasing the remote interfaces and
			// the remote app process being suspended. If the remote app process is suspended first, then COM may cause a hang until the 
			// remote app process is terminated.
			MediaStreamSource mss{ move(m_mss) };
			mss.Starting(m_startingEventToken);
			mss.SampleRequested(m_sampleRequestedEventToken);
			mss.SwitchStreamsRequested(m_switchStreamsRequestedEventToken);
			mss.Closed(m_closedEventToken);
		}
	}
}