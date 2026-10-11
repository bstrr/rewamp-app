/******************************************************************************/
/* This source, or parts thereof, may be used in any software as long the     */
/* license of NostalgicPlayer is keep. See the LICENSE file for more          */
/* information.                                                               */
/*                                                                            */
/* Copyright (c) 2023 Thomas Neumann — NostalgicPlayer (MIT, see LICENSE).    */
/*                                                                            */
/* C++ port for Rewamp of Source/Agents/Players/RonKlaren/RonKlarenWorker.cs  */
/* (upstream commit 3feaf4a5b8cf03794f4b157a99ca434e487b8f54). The order of   */
/* the methods and their bodies follow the C# file; see RonKlarenWorker.h for */
/* the list of what the port changes.                                         */
/******************************************************************************/
#include "RonKlarenWorker.h"

#include <string.h>
#include <algorithm>

namespace ronklaren
{
	namespace
	{
		const int AmigaExecutableHunkSize = 32;
		const int HeaderSize = 32;
		const int MinFileSize = 0xa40;

		// The arpeggio table a voice starts with ("new sbyte[12]")
		const int8_t EmptyArpeggio[12] = { 0 };

		// Guard: a track made only of "end of track" bytes never reaches a
		// note, so ParseTrackData would spin forever (NostalgicPlayer would
		// hang too). No real module comes anywhere near this.
		const int MaxParseIterations = 65536;

		/// Byte of a track; guard: 0xff (end of track) past its end
		inline uint8_t TrackByte(const std::vector<uint8_t>& track, int index)
		{
			return ((index >= 0) && (index < (int)track.size())) ? track[(size_t)index] : 0xff;
		}

		/// Guard: C# would throw on a negative index
		inline int ClampNote(int transposedNote)
		{
			if (transposedNote >= PeriodsLength)
				transposedNote = PeriodsLength - 1;

			if (transposedNote < 0)
				transposedNote = 0;

			return transposedNote;
		}
	}

	/********************************************************************/
	/// <summary>
	/// The part of NostalgicPlayer's ModuleStream the loader uses: big
	/// endian reads, and EndOfStream set when a read or a seek goes past
	/// the end (a short read returns 0, exactly like ReaderStream)
	/// </summary>
	/********************************************************************/
	class RonKlarenWorker::ModuleStream
	{
	public:
		ModuleStream(const uint8_t* d, size_t len) : data(d), length((long)len) {}

		long Length() const { return length; }

		void Seek(long offset, bool fromCurrent)
		{
			long newPos = fromCurrent ? position + offset : offset;
			if (newPos < 0)
				newPos = 0;

			EndOfStream = newPos > length;
			position = newPos > length ? length : newPos;
		}

		int Read(uint8_t* buffer, int count)
		{
			long avail = length - position;
			int read = (int)std::min<long>(avail < 0 ? 0 : avail, count);

			if (read > 0)
				memcpy(buffer, data + position, (size_t)read);

			position += read;
			EndOfStream = read < count;

			return read;
		}

		int ReadSigned(int8_t* buffer, int count)
		{
			return Read((uint8_t*)buffer, count);
		}

		uint8_t Read_UINT8()
		{
			uint8_t b[1];
			return Read(b, 1) == 1 ? b[0] : 0;
		}

		int8_t Read_INT8()
		{
			return (int8_t)Read_UINT8();
		}

		uint16_t Read_B_UINT16()
		{
			uint8_t b[2];
			return Read(b, 2) == 2 ? (uint16_t)((b[0] << 8) | b[1]) : 0;
		}

		int16_t Read_B_INT16()
		{
			return (int16_t)Read_B_UINT16();
		}

		uint32_t Read_B_UINT32()
		{
			uint8_t b[4];
			return Read(b, 4) == 4 ? ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3] : 0;
		}

		int32_t Read_B_INT32()
		{
			return (int32_t)Read_B_UINT32();
		}

		bool EndOfStream = false;

	private:
		const uint8_t* data;
		long length;
		long position = 0;
	};

	/* ---- Identify --------------------------------------------------------- */

	/********************************************************************/
	/// <summary>
	/// Test the file to see if it could be identified
	/// </summary>
	/********************************************************************/
	bool RonKlarenWorker::Identify(const uint8_t* data, size_t length)
	{
		// Check the module size
		if (length < (size_t)MinFileSize)
			return false;

		// Check for Amiga executable hunks
		if ((data[0] != 0x00) || (data[1] != 0x00) || (data[2] != 0x03) || (data[3] != 0xf3))
			return false;

		// Check the identifier
		return memcmp(data + 40, "RON_KLAREN_SOUNDMODULE!", 23) == 0;
	}

	/* ---- Loading ---------------------------------------------------------- */

	/********************************************************************/
	/// <summary>
	/// Will load the file into memory
	/// </summary>
	/********************************************************************/
	bool RonKlarenWorker::Load(const uint8_t* data, size_t length)
	{
		if (!Identify(data, length))
			return false;

		ModuleStream moduleStream(data, length);

		// Load the player code, which is used to search in to find different information
		std::vector<uint8_t> searchBuffer((size_t)MinFileSize, 0);

		moduleStream.Seek(AmigaExecutableHunkSize, false);
		moduleStream.Read(searchBuffer.data(), (int)searchBuffer.size());

		int numberOfSubSongs;
		if (!FindNumberOfSubSongs(searchBuffer, numberOfSubSongs))
			return false;

		uint32_t irqOffset;
		if (!FindSongSpeedAndIrqOffset(searchBuffer, irqOffset))
			return false;

		uint32_t subSongOffset;
		if (!FindSubSongInfo(searchBuffer, moduleStream.Length(), irqOffset, subSongOffset))
			return false;

		uint32_t instrumentOffset, arpeggioOffset;
		if (!FindInstrumentAndArpeggioStartOffsets(searchBuffer, moduleStream.Length(), instrumentOffset, arpeggioOffset))
			return false;

		EnableTrackFeatures(searchBuffer);

		std::vector<uint32_t> subSongTrackOffsets;
		if (!LoadSubSongInfo(moduleStream, numberOfSubSongs, subSongOffset, subSongTrackOffsets))
			return false;

		// Guard: nothing to play (C# would index an empty array)
		if (numberOfSubSongs <= 0)
			return false;

		if (!LoadTrackLists(moduleStream, numberOfSubSongs, subSongTrackOffsets))
			return false;

		if (!LoadTracks(moduleStream))
			return false;

		int instrumentCount, arpeggioCount;
		FindMaxNumberUsedByDifferentEffects(instrumentCount, arpeggioCount);

		// Guard: InitializeVoice selects instrument 0
		if (instrumentCount < 1)
			instrumentCount = 1;

		if (!LoadArpeggios(moduleStream, arpeggioOffset, arpeggioCount))
			return false;

		if (!LoadInstruments(moduleStream, instrumentOffset, instrumentCount))
			return false;

		if (!LoadVibratos(moduleStream))
			return false;

		if (!LoadSamples(moduleStream))
			return false;

		if (!LoadSampleData(moduleStream))
			return false;

		loadedInstruments = instruments;
		loadedSampleData = sampleData;

		InitSound(0);

		// Everything is loaded alright
		return true;
	}

	/* ---- Initialization ---------------------------------------------------- */

	/********************************************************************/
	/// <summary>
	/// Initializes the current song
	/// </summary>
	/********************************************************************/
	void RonKlarenWorker::InitSound(int songNumber)
	{
		if ((songNumber < 0) || (songNumber >= (int)subSongs.size()))
			songNumber = 0;

		// Back to the instruments and waveforms as loaded (see the header)
		instruments = loadedInstruments;

		for (size_t i = 0; i < sampleData.size(); i++)
			std::copy(loadedSampleData[i].begin(), loadedSampleData[i].end(), sampleData[i].begin());

		for (int i = 0; i < 4; i++)
			channelsDone[i] = false;

		HasEndReached = false;

		InitializeSound(songNumber);

		// Remember the song number
		currentSong = songNumber;
	}

	/* ---- Playing ----------------------------------------------------------- */

	/********************************************************************/
	/// <summary>
	/// This is the main player method
	/// </summary>
	/********************************************************************/
	void RonKlarenWorker::Play(IChannel* const channels[4])
	{
		if (playingInfo.SetupNewSubSong)
			SwitchSubSong();
		else
		{
			for (int i = 0; i < 4; i++)
				ProcessVoice(voices[i], channels[i]);
		}
	}

	/********************************************************************/
	/// <summary>
	/// Number of positions in the position list of a voice
	/// </summary>
	/********************************************************************/
	int RonKlarenWorker::PositionListLength(int voice) const
	{
		return voices[voice].PositionList_ != nullptr ? (int)voices[voice].PositionList_->Tracks.size() : 0;
	}

	/* ---- Private methods --------------------------------------------------- */

	/********************************************************************/
	/// <summary>
	/// Try to find the number of sub-songs the module contains
	/// </summary>
	/********************************************************************/
	bool RonKlarenWorker::FindNumberOfSubSongs(const std::vector<uint8_t>& searchBuffer, int& numberOfSubSongs)
	{
		int searchLength = (int)searchBuffer.size();
		int index;

		numberOfSubSongs = 0;

		// Skip all the JMP instructions
		for (index = HeaderSize; index < (searchLength - 6); index += 6)
		{
			if ((searchBuffer[index] != 0x4e) || (searchBuffer[index + 1] != 0xf9))
				break;
		}

		if (index >= (searchLength - 6))
			return false;

		for (; index < (searchLength - 6); index += 8)
		{
			if ((searchBuffer[index] != 0x30) || (searchBuffer[index + 1] != 0x3c))
				break;

			numberOfSubSongs++;
		}

		return numberOfSubSongs != 0;
	}

	/********************************************************************/
	/// <summary>
	/// Try to find the song speed and the offset to the IRQ routine
	/// </summary>
	/********************************************************************/
	bool RonKlarenWorker::FindSongSpeedAndIrqOffset(const std::vector<uint8_t>& searchBuffer, uint32_t& irqOffset)
	{
		int searchLength = (int)searchBuffer.size();
		int index = HeaderSize;

		int initOffset = 0;
		irqOffset = 0;

		int i;
		for (i = 0; i < 2; i++)
		{
			if ((searchBuffer[index] != 0x4e) || (searchBuffer[index + 1] != 0xf9))
				return false;

			initOffset = (int)(((uint32_t)searchBuffer[index + 2] << 24) | ((uint32_t)searchBuffer[index + 3] << 16) | ((uint32_t)searchBuffer[index + 4] << 8) | searchBuffer[index + 5]);

			// Guard: + 1, the test below reads two bytes
			if ((initOffset < 0) || ((initOffset + 1) >= searchLength))
				return false;

			if (((searchBuffer[initOffset] == 0x61) && (searchBuffer[initOffset + 1] == 0x00)) || ((searchBuffer[initOffset] == 0x33) && (searchBuffer[initOffset + 1] == 0xfc)))
				break;

			index += 6;
		}

		if (i == 2)
			return CheckForVBlankPlayer(searchBuffer, irqOffset);

		index = initOffset;

		uint8_t ciaLoValue = 0, ciaHiValue = 0;

		for (; index < (searchLength - 10); index += 2)
		{
			if ((searchBuffer[index] == 0x4e) && (searchBuffer[index + 1] == 0x75))
				break;

			if ((searchBuffer[index] == 0x13) && (searchBuffer[index + 1] == 0xfc))
			{
				uint8_t value = searchBuffer[index + 3];
				uint32_t adr = ((uint32_t)searchBuffer[index + 4] << 24) | ((uint32_t)searchBuffer[index + 5] << 16) | ((uint32_t)searchBuffer[index + 6] << 8) | searchBuffer[index + 7];
				index += 6;

				if (adr == 0xbfd400)
					ciaLoValue = value;
				else if (adr == 0xbfd500)
					ciaHiValue = value;
			}
			else if ((searchBuffer[index] == 0x23) && (searchBuffer[index + 1] == 0xfc))
			{
				uint32_t adr = ((uint32_t)searchBuffer[index + 2] << 24) | ((uint32_t)searchBuffer[index + 3] << 16) | ((uint32_t)searchBuffer[index + 4] << 8) | searchBuffer[index + 5];
				uint32_t destAdr = ((uint32_t)searchBuffer[index + 6] << 24) | ((uint32_t)searchBuffer[index + 7] << 16) | ((uint32_t)searchBuffer[index + 8] << 8) | searchBuffer[index + 9];
				index += 8;

				if (destAdr == 0x00000078)
					irqOffset = adr;
			}
		}

		ciaValue = (uint16_t)((ciaHiValue << 8) | ciaLoValue);

		return (irqOffset != 0) && (irqOffset < (uint32_t)searchLength) && (ciaValue != 0);
	}

	/********************************************************************/
	/// <summary>
	/// Try to find the song speed and the offset to the IRQ routine
	/// in a VBlank player
	/// </summary>
	/********************************************************************/
	bool RonKlarenWorker::CheckForVBlankPlayer(const std::vector<uint8_t>& searchBuffer, uint32_t& irqOffset)
	{
		int searchLength = (int)searchBuffer.size();
		int index = HeaderSize + 6;

		irqOffset = 0;

		if ((searchBuffer[index] != 0x4e) || (searchBuffer[index + 1] != 0xf9))
			return false;

		int playOffset = (int)(((uint32_t)searchBuffer[index + 2] << 24) | ((uint32_t)searchBuffer[index + 3] << 16) | ((uint32_t)searchBuffer[index + 4] << 8) | searchBuffer[index + 5]);

		// Guard: + 1, the test below reads two bytes
		if ((playOffset < 0) || ((playOffset + 1) >= searchLength))
			return false;

		index = playOffset;

		if ((searchBuffer[index] != 0x41) || (searchBuffer[index + 1] != 0xfa))
			return false;

		irqOffset = (uint32_t)playOffset;
		ciaValue = 14187;
		vblankPlayer = true;

		return true;
	}

	/********************************************************************/
	/// <summary>
	/// Try to find the sub-song information
	/// </summary>
	/********************************************************************/
	bool RonKlarenWorker::FindSubSongInfo(const std::vector<uint8_t>& searchBuffer, long moduleLength, uint32_t irqOffset, uint32_t& subSongInfoOffset)
	{
		int searchLength = (int)searchBuffer.size();
		int index;

		subSongInfoOffset = 0;

		// First find where the global address is set into the A0 register
		for (index = (int)irqOffset; index < (searchLength - 2); index += 2)
		{
			if ((searchBuffer[index] == 0x41) && (searchBuffer[index + 1] == 0xfa))
				break;
		}

		if (index >= (searchLength - 2))
			return false;

		// Guard: the displacement is two bytes further
		if ((index + 3) >= searchLength)
			return false;

		uint32_t globalOffset = (uint32_t)(((searchBuffer[index + 2] << 8) | searchBuffer[index + 3]) + index + 2);
		index += 4;

		if (globalOffset >= (uint32_t)moduleLength)
			return false;

		// Find where the sub-song is initialized
		for (; index < (searchLength - 12); index += 2)
		{
			if ((searchBuffer[index] == 0x4e) && ((searchBuffer[index + 1] == 0x73) || (searchBuffer[index + 1] == 0x75)))
				return false;

			if ((searchBuffer[index] == 0x02) && (searchBuffer[index + 1] == 0x40) && (searchBuffer[index + 2] == 0x00) && (searchBuffer[index + 3] == 0x0f) &&
				(searchBuffer[index + 4] == 0x53) && (searchBuffer[index + 5] == 0x40) &&
				(searchBuffer[index + 6] == 0xe9) && (searchBuffer[index + 7] == 0x48) &&
				(searchBuffer[index + 8] == 0x47) && (searchBuffer[index + 9] == 0xf0))
			{
				break;
			}
		}

		if (index >= (searchLength - 12))
			return false;

		subSongInfoOffset = globalOffset + (uint32_t)((searchBuffer[index + 10] << 8) | searchBuffer[index + 11]) + AmigaExecutableHunkSize;

		return subSongInfoOffset < (uint32_t)moduleLength;
	}

	/********************************************************************/
	/// <summary>
	/// Try to find the instrument information and arpeggio tables
	/// </summary>
	/********************************************************************/
	bool RonKlarenWorker::FindInstrumentAndArpeggioStartOffsets(const std::vector<uint8_t>& searchBuffer, long moduleLength, uint32_t& instrumentOffset, uint32_t& arpeggioOffset)
	{
		int searchLength = (int)searchBuffer.size();
		int index;

		instrumentOffset = 0;
		arpeggioOffset = 0;

		for (index = HeaderSize; index < (searchLength - 4); index += 2)
		{
			if ((searchBuffer[index] == 0x0c) && (searchBuffer[index + 1] == 0x12) && (searchBuffer[index + 2] == 0x00))
			{
				if (searchBuffer[index + 3] == 0x82)
				{
					for (; index < (searchLength - 4); index += 2)
					{
						if ((searchBuffer[index] == 0x49) && (searchBuffer[index + 1] == 0xfa))
						{
							instrumentOffset = (uint32_t)(((searchBuffer[index + 2] << 8) | searchBuffer[index + 3]) + index + 2) + AmigaExecutableHunkSize;
							break;
						}
					}
				}

				if (searchBuffer[index + 3] == 0x80)
				{
					for (; index < (searchLength - 4); index += 2)
					{
						if ((searchBuffer[index] == 0x49) && (searchBuffer[index + 1] == 0xfa))
						{
							arpeggioOffset = (uint32_t)(((searchBuffer[index + 2] << 8) | searchBuffer[index + 3]) + index + 2) + AmigaExecutableHunkSize;
							break;
						}
					}
				}
			}

			if ((instrumentOffset != 0) && (arpeggioOffset != 0))
				break;
		}

		if (index >= (searchLength - 4))
			return false;

		return (instrumentOffset < (uint32_t)moduleLength) && (arpeggioOffset < (uint32_t)moduleLength);
	}

	/********************************************************************/
	/// <summary>
	/// Enable/disable track command features
	/// </summary>
	/********************************************************************/
	void RonKlarenWorker::EnableTrackFeatures(const std::vector<uint8_t>& searchBuffer)
	{
		int searchLength = (int)searchBuffer.size();
		int index;

		clearAdsrStateOnPortamento = false;

		for (index = HeaderSize; index < (searchLength - 4); index += 2)
		{
			if ((searchBuffer[index] == 0x0c) && (searchBuffer[index + 1] == 0x12) && (searchBuffer[index + 2] == 0x00))
			{
				if (searchBuffer[index + 3] == 0x81)
				{
					if (index < (searchLength - 10))
					{
						if ((searchBuffer[index + 8] == 0x42) && (searchBuffer[index + 9] == 0x68))
							clearAdsrStateOnPortamento = true;
					}
				}
			}
		}
	}

	/********************************************************************/
	/// <summary>
	/// Load all the track list offsets for each sub-song
	/// </summary>
	/********************************************************************/
	bool RonKlarenWorker::LoadSubSongInfo(ModuleStream& moduleStream, int& numberOfSubSongs, uint32_t subSongInfoOffset, std::vector<uint32_t>& subSongTrackOffsets)
	{
		moduleStream.Seek(subSongInfoOffset, false);

		subSongTrackOffsets.assign((size_t)numberOfSubSongs * 4, 0);

		for (int i = 0; i < numberOfSubSongs; i++)
		{
			for (int j = 0; j < 4; j++)
			{
				uint32_t offset = moduleStream.Read_B_UINT32() + AmigaExecutableHunkSize;
				subSongTrackOffsets[(size_t)i * 4 + (size_t)j] = offset;

				if (offset >= (uint32_t)moduleStream.Length())
				{
					numberOfSubSongs = i;

					return true;
				}
			}

			if (moduleStream.EndOfStream)
				return false;
		}

		return true;
	}

	/********************************************************************/
	/// <summary>
	/// Load all the track lists for each sub-song
	/// </summary>
	/********************************************************************/
	bool RonKlarenWorker::LoadTrackLists(ModuleStream& moduleStream, int numberOfSubSongs, const std::vector<uint32_t>& subSongTrackOffsets)
	{
		subSongs.assign((size_t)numberOfSubSongs, SongInfo());

		for (int i = 0; i < numberOfSubSongs; i++)
		{
			SongInfo& songInfo = subSongs[(size_t)i];

			for (int j = 0; j < 4; j++)
			{
				uint32_t trackListOffset = subSongTrackOffsets[(size_t)i * 4 + (size_t)j];

				moduleStream.Seek(trackListOffset, false);

				if (!LoadSingleTrackList(moduleStream, songInfo.Positions[j].Tracks))
					return false;

				// Guard: InitializeVoice starts on the first entry
				if (songInfo.Positions[j].Tracks.empty())
					return false;
			}
		}

		return true;
	}

	/********************************************************************/
	/// <summary>
	/// Load a single track list and return it
	/// </summary>
	/********************************************************************/
	bool RonKlarenWorker::LoadSingleTrackList(ModuleStream& moduleStream, std::vector<Track>& trackList)
	{
		trackList.clear();

		for (;;)
		{
			int trackOffset = moduleStream.Read_B_INT32();
			if (trackOffset < 0)
				break;

			moduleStream.Seek(2, true);
			int16_t transpose = moduleStream.Read_B_INT16();

			moduleStream.Seek(2, true);
			uint16_t repeatTimes = moduleStream.Read_B_UINT16();

			if (moduleStream.EndOfStream)
				return false;

			Track track;
			track.TrackNumber = trackOffset + AmigaExecutableHunkSize;	// This will be converted later on to a real track number
			track.Transpose = transpose;
			track.NumberOfRepeatTimes = repeatTimes;
			trackList.push_back(track);
		}

		return true;
	}

	/********************************************************************/
	/// <summary>
	/// Load all the tracks and fix the track offsets
	/// </summary>
	/********************************************************************/
	bool RonKlarenWorker::LoadTracks(ModuleStream& moduleStream)
	{
		// Load all the track data into this dictionary.
		// The key is the track offset and the value is the track data
		std::map<int, std::vector<uint8_t>> trackData;

		for (SongInfo& songInfo : subSongs)
		{
			for (int j = 0; j < 4; j++)
			{
				for (Track& track : songInfo.Positions[j].Tracks)
				{
					if (trackData.find(track.TrackNumber) == trackData.end())
					{
						moduleStream.Seek(track.TrackNumber, false);

						std::vector<uint8_t> data;
						if (!LoadSingleTrack(moduleStream, data))
							return false;

						trackData[track.TrackNumber] = data;
					}
				}
			}
		}

		// Now convert the track offset to track numbers in the track lists
		std::map<int, int> offsetToIndexMap;
		BuildOffsetToIndexLists(trackData, tracks, offsetToIndexMap);

		for (SongInfo& songInfo : subSongs)
		{
			for (int j = 0; j < 4; j++)
			{
				for (Track& track : songInfo.Positions[j].Tracks)
					track.TrackNumber = offsetToIndexMap[track.TrackNumber];
			}
		}

		return true;
	}

	/********************************************************************/
	/// <summary>
	/// Load a single track and return it
	/// </summary>
	/********************************************************************/
	bool RonKlarenWorker::LoadSingleTrack(ModuleStream& moduleStream, std::vector<uint8_t>& trackBytes)
	{
		trackBytes.clear();

		for (;;)
		{
			uint8_t byt = moduleStream.Read_UINT8();
			if (moduleStream.EndOfStream)
				return false;

			trackBytes.push_back(byt);

			if (byt == 0xff)
				break;

			int effectCount = FindEffectByteCount(byt);
			if (effectCount < 0)
				return false;		// C#: "Invalid track byte detected" exception

			for (; effectCount > 0; effectCount--)
				trackBytes.push_back(moduleStream.Read_UINT8());
		}

		return true;
	}

	/********************************************************************/
	/// <summary>
	/// Find out how many bytes the given effect uses after the effect
	/// itself. -1 means an invalid track byte
	/// </summary>
	/********************************************************************/
	int RonKlarenWorker::FindEffectByteCount(uint8_t effect)
	{
		if (effect < 128)
			return 1;

		switch (effect)
		{
			case Effect_SetArpeggio:
			case Effect_SetInstrument:
			case Effect_ChangeAdsrSpeed:
				return 1;

			case Effect_SetPortamento:
				return 3;

			case Effect_EndSong:
			case Effect_EndSong2:
			case Effect_EndOfTrack:
				return 0;
		}

		return -1;
	}

	/********************************************************************/
	/// <summary>
	/// Find the max number of instruments and arpeggios by scanning
	/// the track data
	/// </summary>
	/********************************************************************/
	void RonKlarenWorker::FindMaxNumberUsedByDifferentEffects(int& instrumentCount, int& arpeggioCount)
	{
		instrumentCount = 0;
		arpeggioCount = 0;

		for (const std::vector<uint8_t>& track : tracks)
		{
			for (int i = 0; i < (int)track.size(); i++)
			{
				uint8_t effect = track[(size_t)i];

				if (effect == Effect_SetInstrument)
					instrumentCount = std::max(instrumentCount, TrackByte(track, i + 1) + 1);
				else if (effect == Effect_SetArpeggio)
					arpeggioCount = std::max(arpeggioCount, TrackByte(track, i + 1) + 1);

				int count = FindEffectByteCount(effect);
				i += count < 0 ? 0 : count;
			}
		}
	}

	/********************************************************************/
	/// <summary>
	/// Load the arpeggio tables
	/// </summary>
	/********************************************************************/
	bool RonKlarenWorker::LoadArpeggios(ModuleStream& moduleStream, uint32_t arpeggioOffset, int arpeggioCount)
	{
		moduleStream.Seek(arpeggioOffset, false);

		arpeggios.assign((size_t)arpeggioCount, std::vector<int8_t>(12, 0));

		for (int i = 0; i < arpeggioCount; i++)
		{
			if (moduleStream.ReadSigned(arpeggios[(size_t)i].data(), 12) != 12)
				return false;
		}

		return true;
	}

	/********************************************************************/
	/// <summary>
	/// Load the instrument information
	/// </summary>
	/********************************************************************/
	bool RonKlarenWorker::LoadInstruments(ModuleStream& moduleStream, uint32_t instrumentOffset, int instrumentCount)
	{
		moduleStream.Seek(instrumentOffset, false);

		instruments.assign((size_t)instrumentCount, Instrument());

		for (int i = 0; i < instrumentCount; i++)
		{
			Instrument& instr = instruments[(size_t)i];

			instr.SampleNumber = moduleStream.Read_B_INT32() + AmigaExecutableHunkSize;		// This will be converted later on to a real track number
			instr.VibratoNumber = moduleStream.Read_B_INT32() + AmigaExecutableHunkSize;	// This will be converted later on to a real track number
			instr.Type = moduleStream.Read_UINT8() == 0 ? InstrumentType::Synthesis : InstrumentType::Sample;
			instr.PhaseSpeed = moduleStream.Read_UINT8();
			instr.PhaseLengthInWords = moduleStream.Read_UINT8();
			instr.VibratoSpeed = moduleStream.Read_UINT8();
			instr.VibratoDepth = moduleStream.Read_UINT8();
			instr.VibratoDelay = moduleStream.Read_UINT8();

			for (int j = 0; j < 4; j++)
				instr.Adsr[j].Point = moduleStream.Read_UINT8();

			for (int j = 0; j < 4; j++)
				instr.Adsr[j].Increment = moduleStream.Read_UINT8();

			instr.PhaseValue = moduleStream.Read_INT8();
			instr.PhaseDirection = moduleStream.Read_INT8() < 0;
			instr.PhasePosition = moduleStream.Read_UINT8();

			if (moduleStream.EndOfStream)
				return false;

			moduleStream.Seek(7, true);

			if (instr.VibratoSpeed == 0)
				instr.VibratoNumber = -1;
		}

		return true;
	}

	/********************************************************************/
	/// <summary>
	/// Load the vibrato tables
	/// </summary>
	/********************************************************************/
	bool RonKlarenWorker::LoadVibratos(ModuleStream& moduleStream)
	{
		// Load all the vibrato tables into this dictionary.
		// The key is the vibrato offset and the value is the vibrato table
		std::map<int, std::vector<int8_t>> vibratoTables;

		for (Instrument& instr : instruments)
		{
			if (instr.VibratoNumber == -1)
				continue;

			if (vibratoTables.find(instr.VibratoNumber) == vibratoTables.end())
			{
				moduleStream.Seek(instr.VibratoNumber, false);

				uint32_t tableOffset = moduleStream.Read_B_UINT32() + AmigaExecutableHunkSize;
				uint16_t length = moduleStream.Read_B_UINT16();

				if (moduleStream.EndOfStream)
					return false;

				std::vector<int8_t> table((size_t)length * 2, 0);

				moduleStream.Seek((long)tableOffset, false);

				if (moduleStream.ReadSigned(table.data(), (int)table.size()) != (int)table.size())
					return false;

				vibratoTables[instr.VibratoNumber] = table;
			}
		}

		// Now convert the vibrato offset to vibrato numbers in the instruments
		std::map<int, int> offsetToIndexMap;
		BuildOffsetToIndexLists(vibratoTables, vibratos, offsetToIndexMap);

		for (Instrument& instr : instruments)
		{
			if (instr.VibratoNumber != -1)
				instr.VibratoNumber = offsetToIndexMap[instr.VibratoNumber];
		}

		return true;
	}

	/********************************************************************/
	/// <summary>
	/// Load the sample information
	/// </summary>
	/********************************************************************/
	bool RonKlarenWorker::LoadSamples(ModuleStream& moduleStream)
	{
		// Load all the sample information into this dictionary.
		std::map<int, Sample> sampleInfo;

		for (Instrument& instr : instruments)
		{
			if (sampleInfo.find(instr.SampleNumber) == sampleInfo.end())
			{
				moduleStream.Seek(instr.SampleNumber, false);

				Sample sample;

				sample.SampleNumber = moduleStream.Read_B_INT32() + AmigaExecutableHunkSize;	// This will be converted later on to a real sample number
				sample.LengthInWords = moduleStream.Read_B_UINT16();
				sample.PhaseIndex = moduleStream.Read_B_UINT16();

				if (moduleStream.EndOfStream)
					return false;

				sampleInfo[instr.SampleNumber] = sample;
			}
		}

		// Now convert the sample info offset to sample info numbers in the instruments
		std::map<int, int> offsetToIndexMap;
		BuildOffsetToIndexLists(sampleInfo, samples, offsetToIndexMap);

		for (Instrument& instr : instruments)
			instr.SampleNumber = offsetToIndexMap[instr.SampleNumber];

		return true;
	}

	/********************************************************************/
	/// <summary>
	/// Load all the samples
	/// </summary>
	/********************************************************************/
	bool RonKlarenWorker::LoadSampleData(ModuleStream& moduleStream)
	{
		// Load all the sample data into this dictionary.
		std::map<int, std::vector<int8_t>> data;

		for (size_t i = 0; i < samples.size(); i++)
		{
			Sample& sample = samples[i];

			if (data.find(sample.SampleNumber) == data.end())
			{
				moduleStream.Seek(sample.SampleNumber, false);

				std::vector<int8_t> sampData((size_t)sample.LengthInWords * 2, 0);
				int readBytes = moduleStream.ReadSigned(sampData.data(), (int)sampData.size());
				if (readBytes != (int)sampData.size())
					return false;

				data[sample.SampleNumber] = sampData;
			}
		}

		// Now convert the sample data offset to sample data numbers
		std::map<int, int> offsetToIndexMap;
		BuildOffsetToIndexLists(data, sampleData, offsetToIndexMap);

		for (Sample& sample : samples)
			sample.SampleNumber = offsetToIndexMap[sample.SampleNumber];

		return true;
	}

	/********************************************************************/
	/// <summary>
	/// Build array from dictionary and lookup dictionary to help convert
	/// offsets to indexes
	/// </summary>
	/********************************************************************/
	template<typename T>
	void RonKlarenWorker::BuildOffsetToIndexLists(const std::map<int, T>& offsetDictionary, std::vector<T>& result, std::map<int, int>& offsetToIndexMap)
	{
		// std::map is already ordered by key, like the OrderBy() in C#
		result.clear();
		offsetToIndexMap.clear();

		int index = 0;
		for (const auto& pair : offsetDictionary)
		{
			result.push_back(pair.second);
			offsetToIndexMap[pair.first] = index++;
		}
	}

	/********************************************************************/
	/// <summary>
	/// Initialize sound structures
	/// </summary>
	/********************************************************************/
	void RonKlarenWorker::InitializeSound(int songNumber)
	{
		playingInfo = GlobalPlayingInfo();
		playingInfo.GlobalVolume = 0x3f;
		playingInfo.SetupNewSubSong = true;
		playingInfo.SubSongNumber = (uint16_t)(songNumber + 1);

		for (int i = 0; i < 4; i++)
			InitializeVoice(i);

		SetCiaTimerTempo(ciaValue);
	}

	/********************************************************************/
	/// <summary>
	/// Initialize a single voice
	/// </summary>
	/********************************************************************/
	void RonKlarenWorker::InitializeVoice(int voice)
	{
		const PositionList& positionList = subSongs[(size_t)(playingInfo.SubSongNumber - 1)].Positions[voice];

		VoiceInfo v;

		v.ChannelNumber = voice;

		v.PositionList_ = &positionList;
		v.TrackListPosition = 0;
		v.TrackData = &tracks[(size_t)positionList.Tracks[0].TrackNumber];
		v.TrackDataPosition = 0;
		v.TrackRepeatCounter = (uint16_t)(positionList.Tracks[0].NumberOfRepeatTimes - 1);
		v.WaitCounter = 0;

		v.Instrument_ = &instruments[0];
		v.InstrumentNumber = 0;

		v.ArpeggioValues = EmptyArpeggio;
		v.ArpeggioPosition = 0;

		v.CurrentNote = 0;
		v.Transpose = positionList.Tracks[0].Transpose;
		v.Period = 0;

		v.PortamentoEndPeriod = 0;
		v.PortamentoIncrement = 0;

		v.VibratoDelay = 0;
		v.VibratoPosition = 0;

		v.AdsrState = 0;
		v.AdsrSpeed = 0;
		v.AdsrSpeedCounter = 0;
		v.Volume = 0;

		v.PhaseSpeedCounter = 0;

		v.SetHardware = false;
		v.SampleNumber = 0;
		v.SampleData = nullptr;
		v.SampleLength = 0;
		v.SetLoop = false;

		voices[voice] = v;
	}

	/********************************************************************/
	/// <summary>
	/// Initialize a new sub-song
	/// </summary>
	/********************************************************************/
	void RonKlarenWorker::SwitchSubSong()
	{
		if (playingInfo.SubSongNumber == 0)
			playingInfo.SubSongNumber = (uint16_t)(currentSong + 1);

		for (int i = 0; i < 4; i++)
			InitializeVoice(i);

		playingInfo.SetupNewSubSong = false;
		playingInfo.SubSongNumber = 0;
	}

	/********************************************************************/
	/// <summary>
	/// Playing a single voice
	/// </summary>
	/********************************************************************/
	void RonKlarenWorker::ProcessVoice(VoiceInfo& voiceInfo, IChannel* channel)
	{
		if (voiceInfo.SetHardware)
		{
			channel->PlaySample(voiceInfo.SampleNumber, voiceInfo.SampleData, 0, voiceInfo.SampleLength);

			if (voiceInfo.SetLoop)
				channel->SetLoop(0, voiceInfo.SampleLength);

			voiceInfo.SetHardware = false;
		}

		if (voiceInfo.WaitCounter == 0)
			ParseTrackData(voiceInfo, channel);
		else
		{
			voiceInfo.WaitCounter--;

			EffectHandler(voiceInfo, channel);
		}
	}

	/********************************************************************/
	/// <summary>
	/// Parse the next track command
	/// </summary>
	/********************************************************************/
	void RonKlarenWorker::ParseTrackData(VoiceInfo& voiceInfo, IChannel* channel)
	{
		voiceInfo.PortamentoIncrement = 0;

		if (voiceInfo.Instrument_->VibratoDelay != 0)
			voiceInfo.VibratoDelay = (uint8_t)(voiceInfo.Instrument_->VibratoDelay * 4 - 1);

		bool takeOneMore = true;
		int iterations = 0;

		do
		{
			// Guard: see MaxParseIterations
			if (++iterations > MaxParseIterations)
				break;

			if (TrackByte(*voiceInfo.TrackData, voiceInfo.TrackDataPosition) == Effect_SetArpeggio)
			{
				ParseTrackArpeggio(voiceInfo);
			}

			if (TrackByte(*voiceInfo.TrackData, voiceInfo.TrackDataPosition) == Effect_SetPortamento)
			{
				ParseTrackPortamento(voiceInfo);
				takeOneMore = false;
				continue;
			}

			if (TrackByte(*voiceInfo.TrackData, voiceInfo.TrackDataPosition) == Effect_SetInstrument)
				ParseTrackInstrument(voiceInfo, channel);

			if (TrackByte(*voiceInfo.TrackData, voiceInfo.TrackDataPosition) == Effect_EndSong)
			{
				ParseTrackEndSong();
				return;
			}

			if (TrackByte(*voiceInfo.TrackData, voiceInfo.TrackDataPosition) == Effect_ChangeAdsrSpeed)
				ParseTrackChangeAdsrSpeed(voiceInfo);

			if (TrackByte(*voiceInfo.TrackData, voiceInfo.TrackDataPosition) == Effect_EndSong2)
			{
				ParseTrackEndSong();
				return;
			}

			if (TrackByte(*voiceInfo.TrackData, voiceInfo.TrackDataPosition) >= 0x80)
				ParseTrackEndOfTrack(voiceInfo);
			else
				takeOneMore = ParseTrackNewNote(voiceInfo, channel);
		}
		while (takeOneMore);

		channel->SetAmigaPeriod(voiceInfo.Period);
	}

	/********************************************************************/
	/// <summary>
	/// Set new arpeggio table
	/// </summary>
	/********************************************************************/
	void RonKlarenWorker::ParseTrackArpeggio(VoiceInfo& voiceInfo)
	{
		uint8_t arpeggioNumber = TrackByte(*voiceInfo.TrackData, voiceInfo.TrackDataPosition + 1);
		voiceInfo.TrackDataPosition += 2;

		// Guard: the count comes from the same scan, so this always holds
		if (arpeggioNumber < arpeggios.size())
			voiceInfo.ArpeggioValues = arpeggios[arpeggioNumber].data();
	}

	/********************************************************************/
	/// <summary>
	/// Set portamento
	/// </summary>
	/********************************************************************/
	void RonKlarenWorker::ParseTrackPortamento(VoiceInfo& voiceInfo)
	{
		uint8_t endNote = TrackByte(*voiceInfo.TrackData, voiceInfo.TrackDataPosition + 1);
		uint8_t increment = TrackByte(*voiceInfo.TrackData, voiceInfo.TrackDataPosition + 2);
		uint8_t waitCounter = TrackByte(*voiceInfo.TrackData, voiceInfo.TrackDataPosition + 3);
		voiceInfo.TrackDataPosition += 4;

		int transposedNote = ClampNote(endNote + voiceInfo.Transpose);

		if (clearAdsrStateOnPortamento)
			voiceInfo.AdsrState = 0;

		voiceInfo.PortamentoEndPeriod = Periods[transposedNote];
		voiceInfo.PortamentoIncrement = increment;
		voiceInfo.WaitCounter = (uint8_t)(waitCounter * 4 - 1);
	}

	/********************************************************************/
	/// <summary>
	/// Set new instrument
	/// </summary>
	/********************************************************************/
	void RonKlarenWorker::ParseTrackInstrument(VoiceInfo& voiceInfo, IChannel* channel)
	{
		uint8_t instrumentNumber = TrackByte(*voiceInfo.TrackData, voiceInfo.TrackDataPosition + 1);
		voiceInfo.TrackDataPosition += 2;

		// Guard: the count comes from the same scan, so this always holds
		if (instrumentNumber >= instruments.size())
			instrumentNumber = 0;

		voiceInfo.Volume = 0;
		voiceInfo.AdsrState = 0;
		voiceInfo.VibratoPosition = 0;

		voiceInfo.Instrument_ = &instruments[instrumentNumber];
		voiceInfo.InstrumentNumber = instrumentNumber;

		PlaySample(voiceInfo, channel);
	}

	/********************************************************************/
	/// <summary>
	/// End the song
	/// </summary>
	/********************************************************************/
	void RonKlarenWorker::ParseTrackEndSong()
	{
		playingInfo.SetupNewSubSong = true;
		playingInfo.SubSongNumber = 0;

		OnEndReachedOnAllChannels(0);
	}

	/********************************************************************/
	/// <summary>
	/// Change ADSR speed
	/// </summary>
	/********************************************************************/
	void RonKlarenWorker::ParseTrackChangeAdsrSpeed(VoiceInfo& voiceInfo)
	{
		uint8_t speed = TrackByte(*voiceInfo.TrackData, voiceInfo.TrackDataPosition + 1);
		voiceInfo.TrackDataPosition += 2;

		voiceInfo.AdsrSpeed = speed;
	}

	/********************************************************************/
	/// <summary>
	/// End of track
	/// </summary>
	/********************************************************************/
	void RonKlarenWorker::ParseTrackEndOfTrack(VoiceInfo& voiceInfo)
	{
		if (voiceInfo.TrackRepeatCounter == 0)
		{
			voiceInfo.TrackListPosition++;

			if (voiceInfo.TrackListPosition == (int)voiceInfo.PositionList_->Tracks.size())
			{
				voiceInfo.TrackListPosition = 0;

				OnEndReached(voiceInfo.ChannelNumber);
			}

			const Track& track = voiceInfo.PositionList_->Tracks[(size_t)voiceInfo.TrackListPosition];

			voiceInfo.TrackData = &tracks[(size_t)track.TrackNumber];
			voiceInfo.TrackDataPosition = 0;
			voiceInfo.Transpose = track.Transpose;
			voiceInfo.TrackRepeatCounter = (uint16_t)(track.NumberOfRepeatTimes - 1);
		}
		else
		{
			voiceInfo.TrackRepeatCounter--;
			voiceInfo.TrackDataPosition = 0;
			voiceInfo.Transpose = voiceInfo.PositionList_->Tracks[(size_t)voiceInfo.TrackListPosition].Transpose;

			// Battle Squadron title has a track that repeats more than 9000 times,
			// so we need to check for this
			if (voiceInfo.TrackRepeatCounter > 9000)
				OnEndReached(voiceInfo.ChannelNumber);
		}
	}

	/********************************************************************/
	/// <summary>
	/// Parse new note
	/// </summary>
	/********************************************************************/
	bool RonKlarenWorker::ParseTrackNewNote(VoiceInfo& voiceInfo, IChannel* channel)
	{
		uint8_t note = TrackByte(*voiceInfo.TrackData, voiceInfo.TrackDataPosition);
		uint8_t waitCount = TrackByte(*voiceInfo.TrackData, voiceInfo.TrackDataPosition + 1);
		voiceInfo.TrackDataPosition += 2;

		int transposedNote = ClampNote(note + voiceInfo.Transpose);

		voiceInfo.CurrentNote = note;
		voiceInfo.Period = Periods[transposedNote];

		if (waitCount == 0)
			return true;

		voiceInfo.WaitCounter = (uint8_t)(waitCount * 4 - 1);
		voiceInfo.AdsrState = 0;

		PlaySample(voiceInfo, channel);

		return false;
	}

	/********************************************************************/
	/// <summary>
	/// Play instrument sample
	/// </summary>
	/********************************************************************/
	void RonKlarenWorker::PlaySample(VoiceInfo& voiceInfo, IChannel* channel)
	{
		Instrument& instr = *voiceInfo.Instrument_;
		Sample& sample = samples[(size_t)instr.SampleNumber];
		std::vector<int8_t>& data = sampleData[(size_t)sample.SampleNumber];

		voiceInfo.SetHardware = true;
		voiceInfo.SampleNumber = voiceInfo.InstrumentNumber;
		voiceInfo.SampleData = data.data();
		voiceInfo.SampleLength = sample.LengthInWords * 2U;
		voiceInfo.SetLoop = instr.Type == InstrumentType::Synthesis;

		channel->Mute();
	}

	/********************************************************************/
	/// <summary>
	/// Do real time effects
	/// </summary>
	/********************************************************************/
	void RonKlarenWorker::EffectHandler(VoiceInfo& voiceInfo, IChannel* channel)
	{
		Instrument& instr = *voiceInfo.Instrument_;

		DoEffectPhasing(voiceInfo, instr);
		DoEffectPortamento(voiceInfo, channel);
		DoEffectArpeggio(voiceInfo, channel);
		DoEffectVibrato(voiceInfo, channel, instr);
		DoEffectAdsr(voiceInfo, channel, instr);
	}

	/********************************************************************/
	/// <summary>
	/// Do phasing effect
	/// </summary>
	/********************************************************************/
	void RonKlarenWorker::DoEffectPhasing(VoiceInfo& voiceInfo, Instrument& instr)
	{
		if (instr.PhaseSpeed != 0)
		{
			if (voiceInfo.PhaseSpeedCounter == 0)
			{
				voiceInfo.PhaseSpeedCounter = (uint8_t)(instr.PhaseSpeed - 1);

				Sample& sample = samples[(size_t)instr.SampleNumber];
				std::vector<int8_t>& data = sampleData[(size_t)sample.SampleNumber];

				int phaseStart = sample.PhaseIndex - instr.PhaseLengthInWords;
				int phaseIndex = phaseStart + instr.PhasePosition;

				// Guard: >= 0
				if ((phaseIndex >= 0) && (phaseIndex < (int)data.size()))
					data[(size_t)phaseIndex] = instr.PhaseValue;

				if (instr.PhaseDirection)
				{
					instr.PhasePosition--;

					if (instr.PhasePosition == 0)
					{
						instr.PhaseDirection = !instr.PhaseDirection;
						instr.PhaseValue = (int8_t)~instr.PhaseValue;
					}
				}
				else
				{
					instr.PhasePosition++;

					if ((instr.PhaseLengthInWords * 2) == instr.PhasePosition)
					{
						instr.PhaseDirection = !instr.PhaseDirection;
						instr.PhaseValue = (int8_t)~instr.PhaseValue;
					}
				}
			}
			else
				voiceInfo.PhaseSpeedCounter--;
		}
	}

	/********************************************************************/
	/// <summary>
	/// Do portamento
	/// </summary>
	/********************************************************************/
	void RonKlarenWorker::DoEffectPortamento(VoiceInfo& voiceInfo, IChannel* channel)
	{
		if (voiceInfo.PortamentoIncrement != 0)
		{
			uint16_t period = voiceInfo.Period;

			if (period <= voiceInfo.PortamentoEndPeriod)
			{
				period = (uint16_t)(period + voiceInfo.PortamentoIncrement);

				if (period > voiceInfo.PortamentoEndPeriod)
					period = voiceInfo.PortamentoEndPeriod;
			}
			else
			{
				period = (uint16_t)(period - voiceInfo.PortamentoIncrement);

				if (period < voiceInfo.PortamentoEndPeriod)
					period = voiceInfo.PortamentoEndPeriod;
			}

			voiceInfo.Period = period;
			channel->SetAmigaPeriod(period);
		}
	}

	/********************************************************************/
	/// <summary>
	/// Do arpeggio
	/// </summary>
	/********************************************************************/
	void RonKlarenWorker::DoEffectArpeggio(VoiceInfo& voiceInfo, IChannel* channel)
	{
		// Only do arpeggio if no portamento is active
		if (voiceInfo.PortamentoIncrement == 0)
		{
			int transposedNote = ClampNote(voiceInfo.ArpeggioValues[voiceInfo.ArpeggioPosition] + voiceInfo.CurrentNote + voiceInfo.Transpose);

			voiceInfo.Period = Periods[transposedNote];
			channel->SetAmigaPeriod(voiceInfo.Period);

			if (voiceInfo.ArpeggioPosition == 0)
				voiceInfo.ArpeggioPosition = 11;
			else
				voiceInfo.ArpeggioPosition--;
		}
	}

	/********************************************************************/
	/// <summary>
	/// Do vibrato
	/// </summary>
	/********************************************************************/
	void RonKlarenWorker::DoEffectVibrato(VoiceInfo& voiceInfo, IChannel* channel, Instrument& instr)
	{
		if (instr.VibratoSpeed != 0)
		{
			if (voiceInfo.VibratoDelay == 0)
			{
				// Guard: an empty or missing table
				if ((instr.VibratoNumber < 0) || (instr.VibratoNumber >= (int)vibratos.size()) || vibratos[(size_t)instr.VibratoNumber].empty())
					return;

				const std::vector<int8_t>& vibratoTable = vibratos[(size_t)instr.VibratoNumber];

				if (voiceInfo.VibratoPosition >= vibratoTable.size())
					voiceInfo.VibratoPosition = (uint16_t)(vibratoTable.size() - 1);

				uint16_t period = (uint16_t)(vibratoTable[voiceInfo.VibratoPosition] * instr.VibratoDepth + voiceInfo.Period);
				channel->SetAmigaPeriod(period);

				int newPosition = voiceInfo.VibratoPosition - instr.VibratoSpeed;
				if (newPosition < 0)
					newPosition = (int)vibratoTable.size() - 1;

				voiceInfo.VibratoPosition = (uint16_t)newPosition;
			}
			else
				voiceInfo.VibratoDelay--;
		}
	}

	/********************************************************************/
	/// <summary>
	/// Do ADSR
	/// </summary>
	/********************************************************************/
	void RonKlarenWorker::DoEffectAdsr(VoiceInfo& voiceInfo, IChannel* channel, Instrument& instr)
	{
		voiceInfo.AdsrSpeedCounter = (int8_t)(voiceInfo.AdsrSpeedCounter - 1);

		if (voiceInfo.AdsrSpeedCounter < 0)
		{
			voiceInfo.AdsrSpeedCounter = (int8_t)voiceInfo.AdsrSpeed;

			AdsrPoint& point = instr.Adsr[voiceInfo.AdsrState & 3];

			auto NextState = [&voiceInfo]()
			{
				if (voiceInfo.AdsrState < 3)
					voiceInfo.AdsrState++;
			};

			int volume = voiceInfo.Volume;

			if (volume > point.Point)
			{
				volume -= point.Increment;

				if (volume <= point.Point)
				{
					volume = point.Point;
					NextState();
				}
			}
			else
			{
				volume += point.Increment;

				if (volume >= playingInfo.GlobalVolume)
				{
					volume = playingInfo.GlobalVolume;
					NextState();
				}
				else if (volume >= point.Point)
				{
					volume = point.Point;
					NextState();
				}
			}

			voiceInfo.Volume = (uint8_t)volume;
			channel->SetAmigaVolume((uint16_t)(volume & 0x3f));
		}
	}

	/* ---- ModulePlayerWithSubSongDurationAgentBase -------------------------- */

	/********************************************************************/
	/// <summary>
	/// Tell that a channel has reached its end. When every channel has,
	/// the module has ended
	/// </summary>
	/********************************************************************/
	void RonKlarenWorker::OnEndReached(int channel)
	{
		channelsDone[channel] = true;

		for (int i = 0; i < 4; i++)
		{
			if (!channelsDone[i])
				return;
		}

		HasEndReached = true;

		for (int i = 0; i < 4; i++)
			channelsDone[i] = false;
	}

	/********************************************************************/
	/// <summary>
	/// Tell that the module has ended on all channels
	/// </summary>
	/********************************************************************/
	void RonKlarenWorker::OnEndReachedOnAllChannels(int restartPosition)
	{
		(void)restartPosition;	// Only used for NostalgicPlayer's position display

		for (int i = 0; i < 4; i++)
			channelsDone[i] = true;

		OnEndReached(0);
	}
}
