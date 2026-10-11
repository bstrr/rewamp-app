/******************************************************************************/
/* This source, or parts thereof, may be used in any software as long the     */
/* license of NostalgicPlayer is keep. See the LICENSE file for more          */
/* information.                                                               */
/*                                                                            */
/* Copyright (c) 2023 Thomas Neumann — NostalgicPlayer (MIT, see LICENSE).    */
/*                                                                            */
/* C++ port for Rewamp of Source/Agents/Players/RonKlaren/RonKlarenWorker.cs  */
/* (upstream commit 3feaf4a5b8cf03794f4b157a99ca434e487b8f54).                */
/*                                                                            */
/* What changed in the port, and only that:                                   */
/*  - the NostalgicPlayer kit (ModuleStream, IChannel, the duration base      */
/*    class) is replaced by the minimal equivalents below: a big-endian       */
/*    reader with the same end-of-stream rules, an IChannel interface with    */
/*    the five calls the player makes, and the per-channel "end reached"      */
/*    bookkeeping of ModulePlayerWithSubSongDurationAgentBase;                */
/*  - exceptions become a false return from Load();                           */
/*  - indexes that would throw IndexOutOfRange in C# on a malformed module    */
/*    are clamped instead (each one is marked "guard");                       */
/*  - InitSound() restores the instruments and the synthesis waveforms as     */
/*    they were LOADED (the phasing effect rewrites both while playing).      */
/*    NostalgicPlayer gets the same effect from the snapshot it takes at the  */
/*    start of each sub-song; the host here restarts by calling InitSound().  */
/*  - the module-information strings and sample listing (UI only) are left    */
/*    out; the host builds its own from the accessors.                        */
/******************************************************************************/
#ifndef RONKLAREN_WORKER_H
#define RONKLAREN_WORKER_H

#include <stddef.h>
#include <stdint.h>
#include <map>
#include <vector>

#include "RonKlarenContainers.h"

namespace ronklaren
{
	/// The calls the player makes on an Amiga channel (subset of the
	/// NostalgicPlayer IChannel interface, same semantics)
	class IChannel
	{
	public:
		virtual ~IChannel() {}

		/// Start playing `length` bytes of `data` from `startOffset`, once
		virtual void PlaySample(int16_t sampleNumber, const int8_t* data, uint32_t startOffset, uint32_t length) = 0;

		/// Loop part of the sample set by the last PlaySample
		virtual void SetLoop(uint32_t startOffset, uint32_t length) = 0;

		/// Paula period (0 = no frequency)
		virtual void SetAmigaPeriod(uint32_t period) = 0;

		/// Paula volume 0-64
		virtual void SetAmigaVolume(uint16_t vol) = 0;

		/// Stop the channel
		virtual void Mute() = 0;
	};

	/// Main worker class
	class RonKlarenWorker
	{
	public:
		/// Test the file to see if it could be identified
		static bool Identify(const uint8_t* data, size_t length);

		/// Will load the file into memory
		bool Load(const uint8_t* data, size_t length);

		/// Initializes the given sub-song (0-based)
		void InitSound(int songNumber);

		/// This is the main player method: one tick, at PlayingFrequency()
		void Play(IChannel* const channels[4]);

		/// Set by Play() when the song has ended (all voices have looped, or
		/// an "end song" command was met). The host clears it.
		bool HasEndReached = false;

		int NumberOfSubSongs() const { return (int)subSongs.size(); }
		float PlayingFrequency() const { return playingFrequency; }
		uint16_t CiaValue() const { return ciaValue; }
		bool IsVBlankPlayer() const { return vblankPlayer; }
		int NumberOfTracks() const { return (int)tracks.size(); }
		int NumberOfInstruments() const { return (int)instruments.size() - 1; }	// First is "empty"
		int PositionListLength(int voice) const;
		int TrackListPosition(int voice) const { return voices[voice].TrackListPosition; }
		const VoiceInfo& Voice(int voice) const { return voices[voice]; }

	private:
		bool FindNumberOfSubSongs(const std::vector<uint8_t>& searchBuffer, int& numberOfSubSongs);
		bool FindSongSpeedAndIrqOffset(const std::vector<uint8_t>& searchBuffer, uint32_t& irqOffset);
		bool CheckForVBlankPlayer(const std::vector<uint8_t>& searchBuffer, uint32_t& irqOffset);
		bool FindSubSongInfo(const std::vector<uint8_t>& searchBuffer, long moduleLength, uint32_t irqOffset, uint32_t& subSongInfoOffset);
		bool FindInstrumentAndArpeggioStartOffsets(const std::vector<uint8_t>& searchBuffer, long moduleLength, uint32_t& instrumentOffset, uint32_t& arpeggioOffset);
		void EnableTrackFeatures(const std::vector<uint8_t>& searchBuffer);

		class ModuleStream;
		bool LoadSubSongInfo(ModuleStream& moduleStream, int& numberOfSubSongs, uint32_t subSongInfoOffset, std::vector<uint32_t>& subSongTrackOffsets);
		bool LoadTrackLists(ModuleStream& moduleStream, int numberOfSubSongs, const std::vector<uint32_t>& subSongTrackOffsets);
		bool LoadSingleTrackList(ModuleStream& moduleStream, std::vector<Track>& trackList);
		bool LoadTracks(ModuleStream& moduleStream);
		bool LoadSingleTrack(ModuleStream& moduleStream, std::vector<uint8_t>& trackBytes);
		static int FindEffectByteCount(uint8_t effect);
		void FindMaxNumberUsedByDifferentEffects(int& instrumentCount, int& arpeggioCount);
		bool LoadArpeggios(ModuleStream& moduleStream, uint32_t arpeggioOffset, int arpeggioCount);
		bool LoadInstruments(ModuleStream& moduleStream, uint32_t instrumentOffset, int instrumentCount);
		bool LoadVibratos(ModuleStream& moduleStream);
		bool LoadSamples(ModuleStream& moduleStream);
		bool LoadSampleData(ModuleStream& moduleStream);

		void InitializeSound(int songNumber);
		void InitializeVoice(int voice);
		void SwitchSubSong();
		void ProcessVoice(VoiceInfo& voiceInfo, IChannel* channel);
		void ParseTrackData(VoiceInfo& voiceInfo, IChannel* channel);
		void ParseTrackArpeggio(VoiceInfo& voiceInfo);
		void ParseTrackPortamento(VoiceInfo& voiceInfo);
		void ParseTrackInstrument(VoiceInfo& voiceInfo, IChannel* channel);
		void ParseTrackEndSong();
		void ParseTrackChangeAdsrSpeed(VoiceInfo& voiceInfo);
		void ParseTrackEndOfTrack(VoiceInfo& voiceInfo);
		bool ParseTrackNewNote(VoiceInfo& voiceInfo, IChannel* channel);
		void PlaySample(VoiceInfo& voiceInfo, IChannel* channel);
		void EffectHandler(VoiceInfo& voiceInfo, IChannel* channel);
		void DoEffectPhasing(VoiceInfo& voiceInfo, Instrument& instr);
		void DoEffectPortamento(VoiceInfo& voiceInfo, IChannel* channel);
		void DoEffectArpeggio(VoiceInfo& voiceInfo, IChannel* channel);
		void DoEffectVibrato(VoiceInfo& voiceInfo, IChannel* channel, Instrument& instr);
		void DoEffectAdsr(VoiceInfo& voiceInfo, IChannel* channel, Instrument& instr);

		// ModulePlayerWithSubSongDurationAgentBase / ModulePlayerAgentBase
		void SetCiaTimerTempo(uint16_t ciaTimerValue) { playingFrequency = 709379.0f / ciaTimerValue; }
		void OnEndReached(int channel);
		void OnEndReachedOnAllChannels(int restartPosition);

		template<typename T>
		static void BuildOffsetToIndexLists(const std::map<int, T>& offsetDictionary, std::vector<T>& result, std::map<int, int>& offsetToIndexMap);

		bool clearAdsrStateOnPortamento = false;

		uint16_t ciaValue = 0;
		bool vblankPlayer = false;
		int currentSong = 0;
		float playingFrequency = 50.0f;
		bool channelsDone[4] = { false, false, false, false };

		std::vector<SongInfo> subSongs;
		std::vector<std::vector<uint8_t>> tracks;
		std::vector<std::vector<int8_t>> arpeggios;
		std::vector<std::vector<int8_t>> vibratos;
		std::vector<Instrument> instruments;
		std::vector<Sample> samples;
		std::vector<std::vector<int8_t>> sampleData;

		// As loaded — restored by InitSound()
		std::vector<Instrument> loadedInstruments;
		std::vector<std::vector<int8_t>> loadedSampleData;

		GlobalPlayingInfo playingInfo;
		VoiceInfo voices[4];
	};
}

#endif
