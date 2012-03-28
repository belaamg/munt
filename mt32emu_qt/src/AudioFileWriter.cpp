/* Copyright (C) 2011, 2012 Jerome Fisher, Sergey V. Mikayev
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <QMessageBox>
#include <QFileDialog>

#include "AudioFileWriter.h"

static const unsigned int FRAME_SIZE = 4; // Stereo, 16-bit
static const unsigned char WAVE_HEADER[] = {
	0x52, 0x49, 0x46, 0x46, 0x00, 0x00, 0x00, 0x00, 0x57, 0x41, 0x56, 0x45, 0x66, 0x6D, 0x74, 0x20,
	0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x44, 0xAC, 0x00, 0x00, 0x10, 0xB1, 0x02, 0x00,
	0x04, 0x00, 0x10, 0x00, 0x64, 0x61, 0x74, 0x61, 0x00, 0x00, 0x00, 0x00
};

AudioFileWriter::AudioFileWriter() :
	synth(NULL),
	buffer(NULL),
	parser(NULL)
	{}

AudioFileWriter::~AudioFileWriter() {
	stop();
	if (!realtimeMode) {
		delete parser;
		if (synth != NULL) {
			synth->close();
			delete synth;
		}
	}
	delete[] buffer;
}

void AudioFileWriter::convertMIDIFile(QString useOutFileName, QString useMIDIFileName, unsigned int useBufferSize) {
	if (useOutFileName.isEmpty() || useMIDIFileName.isEmpty()) return;
	if (synth != NULL) {
		synth->close();
		delete synth;
	}
	delete parser;
	delete[] buffer;
	synth = new QSynth(this);
	if (!synth->open()) {
		synth->close();
		delete synth;
		qDebug() << "AudioFileWriter: Can't open synth";
		QMessageBox::critical(NULL, "Error", "Failed to open synth");
		return;
	}
	sampleRate = MT32Emu::SAMPLE_RATE;
	bufferSize = useBufferSize;
	latency = 0;
	outFileName = useOutFileName;
	buffer = new qint16[2 * bufferSize];
	midiFileName = useMIDIFileName;
	parser = new MidiParser;
	realtimeMode = false;
	stopProcessing = false;
	if (!parser->parse(midiFileName)) {
		qDebug() << "AudioFileWriter: Error parsing MIDI file:" << midiFileName;
		QMessageBox::critical(NULL, "Error", "Error encountered while loading MIDI file");
		return;
	}
	QThread::start();
}

void AudioFileWriter::startRealtimeProcessing(QSynth *useSynth, unsigned int useSampleRate, QString useOutFileName, unsigned int useBufferSize, MasterClockNanos useLatency) {
	if (useOutFileName.isEmpty()) return;
	delete[] buffer;
	synth = useSynth;
	sampleRate = useSampleRate;
	bufferSize = useBufferSize;
	latency = useLatency;
	outFileName = useOutFileName;
	buffer = new qint16[2 * bufferSize];
	realtimeMode = true;
	stopProcessing = false;
	QThread::start();
}

void AudioFileWriter::stop() {
	stopProcessing = true;
	wait();
}

void AudioFileWriter::run() {
	QFile file(outFileName);
	bool waveMode = false;
	if (outFileName.endsWith(".wav")) waveMode = true;
	if (!file.open(QIODevice::WriteOnly)) {
		qDebug() << "AudioFileWriter: Can't open file for writing:" << outFileName;
		return;
	}
	if (waveMode) file.seek(44);
	MasterClockNanos firstSampleNanos;
	MasterClockNanos midiTick;
	MasterClockNanos midiNanos;
	QVector<MidiEvent> midiEvents;
	int midiEventIx;
	if (realtimeMode) {
		firstSampleNanos = MasterClock::getClockNanos();
	} else {
		firstSampleNanos = 0;
		midiEvents = parser->getMIDIEvents();
		midiTick = parser->getMidiTick();
		midiNanos = 0;
		midiEventIx = 0;
	}
	qDebug() << "AudioFileWriter: Rendering started";
	while (!stopProcessing) {
		unsigned int frameCount;
		if (realtimeMode) {
			frameCount = sampleRate * (MasterClock::getClockNanos() - firstSampleNanos) / MasterClock::NANOS_PER_SECOND;
			if (frameCount < bufferSize) {
				usleep(1000000 * (bufferSize - frameCount) / sampleRate);
				continue;
			}
		} else {
			while (midiEventIx < midiEvents.count()) {
				const MidiEvent &e = midiEvents.at(midiEventIx);
				bool eventPushed = true;
				MasterClockNanos nextEventNanos = midiNanos + e.getTimestamp() * midiTick;
				frameCount = (uint)(sampleRate * (nextEventNanos - firstSampleNanos) / MasterClock::NANOS_PER_SECOND);
				if (bufferSize < frameCount) {
					frameCount = bufferSize;
					break;
				}
				switch (e.getType()) {
					case SHORT_MESSAGE:
						eventPushed = synth->pushMIDIShortMessage(e.getShortMessage(), nextEventNanos);
						break;
					case SYSEX:
						eventPushed = synth->pushMIDISysex(e.getSysexData(), e.getSysexLen(), nextEventNanos);
						break;
					case SET_TEMPO:
						midiTick = parser->getMidiTick(e.getShortMessage());
						break;
				}
				if (!eventPushed) {
					qDebug() << "AudioFileWriter: MIDI buffer overflow, midiNanos:" << midiNanos;
					break;
				}
				midiNanos = nextEventNanos;
				midiEventIx++;
			}
			if (midiEvents.count() <= midiEventIx) {
				frameCount += bufferSize;
				stopProcessing = true;
			}
		}
		while (frameCount > 0) {
			unsigned int framesToRender = qMin(bufferSize, frameCount);
			unsigned int framesRendered = synth->render(buffer, framesToRender, firstSampleNanos - latency, sampleRate);
			qint64 bytesToWrite = framesRendered * FRAME_SIZE;
			char *bufferPos = (char *)buffer;
			while (bytesToWrite > 0) {
				qint64 bytesWritten = file.write(bufferPos, bytesToWrite);
				if (bytesWritten == -1) {
					qDebug() << "AudioFileWriter: error writing into the audio file:" << file.errorString();
					file.close();
					return;
				}
				bytesToWrite -= bytesWritten;
				bufferPos += bytesWritten;
			}
			firstSampleNanos += MasterClock::NANOS_PER_SECOND * framesRendered / sampleRate;
			frameCount -= framesRendered;
			if (!realtimeMode) qDebug() << "AudioFileWriter: Rendering time:" << (double)firstSampleNanos / MasterClock::NANOS_PER_SECOND;
		}
	}
	qDebug() << "AudioFileWriter: Rendering finished";
	if (waveMode) {
		unsigned char *charBuffer = (unsigned char *)buffer;
		memcpy(charBuffer, WAVE_HEADER, 44);
		unsigned long fileSize = (unsigned long)file.size();
		qToLittleEndian(fileSize - 8, charBuffer + 4);
		qToLittleEndian(fileSize - 44, charBuffer + 40);
		qToLittleEndian(sampleRate, charBuffer + 24);
		qToLittleEndian(sampleRate * FRAME_SIZE, charBuffer + 28);
		file.seek(0);
		file.write((char *)charBuffer, 44);
	}
	file.close();
}