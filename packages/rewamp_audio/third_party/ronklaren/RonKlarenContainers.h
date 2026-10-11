/******************************************************************************/
/* This source, or parts thereof, may be used in any software as long the     */
/* license of NostalgicPlayer is keep. See the LICENSE file for more          */
/* information.                                                               */
/*                                                                            */
/* Copyright (c) 2023 Thomas Neumann — NostalgicPlayer (MIT, see LICENSE).    */
/*                                                                            */
/* C++ port for Rewamp of Source/Agents/Players/RonKlaren/Containers (all .cs) and */
/* Tables.cs (upstream commit 3feaf4a5b8cf03794f4b157a99ca434e487b8f54).      */
/* One header instead of one file per class; names and fields unchanged.     */
/******************************************************************************/
#ifndef RONKLAREN_CONTAINERS_H
#define RONKLAREN_CONTAINERS_H

#include <stdint.h>
#include <vector>

namespace ronklaren
{
	/// Different effects (Effect.cs)
	enum Effect
	{
		Effect_SetArpeggio = 0x80,
		Effect_SetPortamento = 0x81,
		Effect_SetInstrument = 0x82,
		Effect_EndSong = 0x83,
		Effect_ChangeAdsrSpeed = 0x84,
		Effect_EndSong2 = 0x85,
		Effect_EndOfTrack = 0xff
	};

	/// Different instrument types (InstrumentType.cs)
	enum class InstrumentType
	{
		Sample,
		Synthesis
	};

	/// A single ADSR (AdsrPoint.cs)
	struct AdsrPoint
	{
		uint8_t Point = 0;
		uint8_t Increment = 0;
	};

	/// Holds information about a single instrument (Instrument.cs)
	struct Instrument
	{
		int SampleNumber = 0;
		int VibratoNumber = 0;
		InstrumentType Type = InstrumentType::Sample;
		uint8_t PhaseSpeed = 0;
		uint8_t PhaseLengthInWords = 0;
		uint8_t VibratoSpeed = 0;
		uint8_t VibratoDepth = 0;
		uint8_t VibratoDelay = 0;
		AdsrPoint Adsr[4];
		int8_t PhaseValue = 0;
		bool PhaseDirection = false;
		uint8_t PhasePosition = 0;
	};

	/// Holds information about a single sample (Sample.cs)
	struct Sample
	{
		int SampleNumber = 0;
		uint16_t LengthInWords = 0;
		uint16_t PhaseIndex = 0;
	};

	/// Holds information about a track (Track.cs)
	struct Track
	{
		int TrackNumber = 0;
		int16_t Transpose = 0;
		uint16_t NumberOfRepeatTimes = 0;
	};

	/// Holds all the tracks for a single voice (PositionList.cs)
	struct PositionList
	{
		std::vector<Track> Tracks;
	};

	/// Holds information about a sub-song (SongInfo.cs)
	struct SongInfo
	{
		PositionList Positions[4];
	};

	/// Holds global information about the playing state (GlobalPlayingInfo.cs)
	struct GlobalPlayingInfo
	{
		uint8_t GlobalVolume = 0;

		bool SetupNewSubSong = false;
		uint16_t SubSongNumber = 0;
	};

	/// Holds playing information for a single voice (VoiceInfo.cs)
	///
	/// The C# class holds object references; here they are pointers into the
	/// worker's own vectors, which are never resized after loading.
	struct VoiceInfo
	{
		int ChannelNumber = 0;

		const PositionList* PositionList_ = nullptr;
		int TrackListPosition = 0;
		const std::vector<uint8_t>* TrackData = nullptr;
		int TrackDataPosition = 0;
		uint16_t TrackRepeatCounter = 0;
		uint8_t WaitCounter = 0;

		Instrument* Instrument_ = nullptr;
		int16_t InstrumentNumber = 0;

		const int8_t* ArpeggioValues = nullptr;		// Always 12 entries
		uint16_t ArpeggioPosition = 0;

		uint8_t CurrentNote = 0;
		int16_t Transpose = 0;
		uint16_t Period = 0;

		uint16_t PortamentoEndPeriod = 0;
		uint8_t PortamentoIncrement = 0;

		uint8_t VibratoDelay = 0;
		uint16_t VibratoPosition = 0;

		uint16_t AdsrState = 0;
		uint8_t AdsrSpeed = 0;
		int8_t AdsrSpeedCounter = 0;
		uint8_t Volume = 0;

		uint8_t PhaseSpeedCounter = 0;

		// These are used to hold the Amiga hardware registers.
		// It is needed, because the original player first set
		// the hardware registers at the next frame. This is
		// needed to avoid some clicks in the sound
		bool SetHardware = false;
		int16_t SampleNumber = 0;
		const int8_t* SampleData = nullptr;
		uint32_t SampleLength = 0;
		bool SetLoop = false;
	};

	/// Different tables needed (Tables.cs)
	static const uint16_t Periods[] =
	{
		6848, 6464, 6096, 5760, 5424, 5120, 4832, 4560, 4304, 4064, 3840, 3616,
		3424, 3232, 3048, 2880, 2712, 2560, 2416, 2280, 2152, 2032, 1920, 1808,
		1712, 1616, 1524, 1440, 1356, 1280, 1208, 1140, 1076, 1016,  960,  904,
		 856,  808,  762,  720,  678,  640,  604,  570,  538,  508,  480,  452,
		 428,  404,  381,  360,  339,  320,  302,  285,  269,  254,  240,  226,
		 214,  202,  190,  180,  170,  160,  151,  143,  135,  127
	};
	static const int PeriodsLength = (int)(sizeof(Periods) / sizeof(Periods[0]));
}

#endif
