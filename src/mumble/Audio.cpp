// Copyright 2005-2020 The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "Audio.h"

#include "AudioInput.h"
#include "AudioOutput.h"
#include "CELTCodec.h"
#ifdef USE_OPUS
# include "OpusCodec.h"
#endif
#include "PacketDataStream.h"
#include "Log.h"

#include <QtCore/QObject>

// We define a global macro called 'g'. This can lead to issues when included code uses 'g' as a type or parameter name (like protobuf 3.7 does). As such, for now, we have to make this our last include.
#include "Global.h"

class CodecInit : public DeferInit {
	public:
		void initialize();
		void destroy();
};

#define DOUBLE_RAND (rand()/static_cast<double>(RAND_MAX))

LoopUser LoopUser::lpLoopy;
CodecInit ciInit;

void CodecInit::initialize() {
#ifdef USE_OPUS
	OpusCodec *oCodec = new OpusCodec();
	if (oCodec->isValid()) {
		oCodec->report();
		g.oCodec = oCodec;
	} else {
		Log::logOrDefer(Log::CriticalError, QObject::tr("CodecInit: Failed to load Opus, it will not be available for encoding/decoding audio."));
		delete oCodec;
	}
#endif

	if (g.s.bDisableCELT) {
		// Kill switch for CELT activated. Do not initialize it.
		return;
	}

	CELTCodec *codec = nullptr;

#ifdef USE_SBCELT
	codec = new CELTCodecSBCELT();
	if (codec->isValid()) {
		codec->report();
		g.qmCodecs.insert(codec->bitstreamVersion(), codec);
	} else {
		delete codec;
	}
#else
	codec = new CELTCodec070(QLatin1String("0.7.0"));
	if (codec->isValid()) {
		codec->report();
		g.qmCodecs.insert(codec->bitstreamVersion(), codec);
	} else {
		delete codec;
		codec = new CELTCodec070(QLatin1String("0.0.0"));
		if (codec->isValid()) {
			codec->report();
			g.qmCodecs.insert(codec->bitstreamVersion(), codec);
		} else {
			delete codec;
		}
	}
#endif
}

void CodecInit::destroy() {
#ifdef USE_OPUS
	delete g.oCodec;
#endif

	foreach(CELTCodec *codec, g.qmCodecs)
		delete codec;
	g.qmCodecs.clear();
}

LoopUser::LoopUser() {
	qsName = QLatin1String("Loopy");
	uiSession = 0;
	iId = 0;
	bMute = bDeaf = bSuppress = false;
	bLocalIgnore = bLocalMute = bSelfDeaf = false;
	tsState = Settings::Passive;
	cChannel = nullptr;
	qetTicker.start();
	qetLastFetch.start();
}

void LoopUser::addFrame(const QByteArray &packet) {
	if (DOUBLE_RAND < g.s.dPacketLoss) {
		qWarning("Drop");
		return;
	}

	{
		QMutexLocker l(&qmLock);
		bool restart = (qetLastFetch.elapsed() > 100);

		double time = qetTicker.elapsed();

		double r;
		if (restart)
			r = 0.0;
		else
			r = DOUBLE_RAND * g.s.dMaxPacketDelay;

		qmPackets.insert(static_cast<float>(time + r), packet);
	}

	// Restart check
	if (qetLastFetch.elapsed() > 100) {
		AudioOutputPtr ao = g.ao;
		if (ao) {
			MessageHandler::UDPMessageType msgType = static_cast<MessageHandler::UDPMessageType>((packet.at(0) >> 5) & 0x7);
			ao->addFrameToBuffer(this, QByteArray(), 0, msgType);
		}
	}

}

void LoopUser::fetchFrames() {
	QMutexLocker l(&qmLock);

	AudioOutputPtr ao(g.ao);
	if (!ao || qmPackets.isEmpty()) {
		return;
	}

	double cmp = qetTicker.elapsed();

	QMultiMap<float, QByteArray>::iterator i = qmPackets.begin();

	while (i != qmPackets.end()) {
		if (i.key() > cmp)
			break;

		int iSeq;
		const QByteArray &data = i.value();
		PacketDataStream pds(data.constData(), data.size());

		unsigned int msgFlags = static_cast<unsigned int>(pds.next());

		pds >> iSeq;

		QByteArray qba;
		qba.reserve(pds.left() + 1);
		qba.append(static_cast<char>(msgFlags));
		qba.append(pds.dataBlock(pds.left()));

		MessageHandler::UDPMessageType msgType = static_cast<MessageHandler::UDPMessageType>((msgFlags >> 5) & 0x7);

		ao->addFrameToBuffer(this, qba, iSeq, msgType);
		i = qmPackets.erase(i);
	}

	qetLastFetch.restart();
}

RecordUser::RecordUser() : LoopUser() {
	qsName = QLatin1String("Recorder");
}

RecordUser::~RecordUser() {
	AudioOutputPtr ao = g.ao;
	if (ao)
		ao->removeBuffer(this);
}

void RecordUser::addFrame(const QByteArray &packet) {
	AudioOutputPtr ao(g.ao);
	if (!ao)
		return;

	int iSeq;
	PacketDataStream pds(packet.constData(), packet.size());

	unsigned int msgFlags = static_cast<unsigned int>(pds.next());

	pds >> iSeq;

	QByteArray qba;
	qba.reserve(pds.left() + 1);
	qba.append(static_cast<char>(msgFlags));
	qba.append(pds.dataBlock(pds.left()));

	MessageHandler::UDPMessageType msgType = static_cast<MessageHandler::UDPMessageType>((msgFlags >> 5) & 0x7);

	ao->addFrameToBuffer(this, qba, iSeq, msgType);
}

void Audio::startOutput(const QString &output) {
	g.ao = AudioOutputRegistrar::newFromChoice(output);
	if (g.ao)
		g.ao->start(QThread::HighPriority);
}

void Audio::stopOutput() {
	// Take a copy of the global AudioOutput shared pointer
	// to keep a reference around.
	AudioOutputPtr ao = g.ao;

	// Reset the global AudioOutput shared pointer to the null pointer.
	g.ao.reset();

	// Wait until our copy of the AudioOutput shared pointer (ao)
	// is the only one left.
	while (ao.get() && ! ao.unique()) {
		QThread::yieldCurrentThread();
	}

	// Reset our copy of the AudioOutput shared pointer.
	// This causes the AudioOutput destructor to be called
	// right here in this function, on the main thread.
	// Our audio backends expect this to happen.
	//
	// One such example is PulseAudioInput, whose destructor
	// takes the PulseAudio mainloop lock. If the destructor
	// is called inside one of the PulseAudio callbacks that
	// take copies of g.ai, the destructor will try to also
	// take the mainloop lock, causing an abort().
	ao.reset();
}

void Audio::startInput(const QString &input) {
	g.ai = AudioInputRegistrar::newFromChoice(input);
	if (g.ai)
		g.ai->start(QThread::HighestPriority);
}

void Audio::stopInput() {
	// Take a copy of the global AudioInput shared pointer
	// to keep a reference around.
	AudioInputPtr ai = g.ai;

	// Reset the global AudioInput shared pointer to the null pointer.
	g.ai.reset();

	// Wait until our copy of the AudioInput shared pointer (ai)
	// is the only one left.
	while (ai.get() && ! ai.unique()) {
		QThread::yieldCurrentThread();
	}

	// Reset our copy of the AudioInput shared pointer.
	// This causes the AudioInput destructor to be called
	// right here in this function, on the main thread.
	// Our audio backends expect this to happen.
	//
	// One such example is PulseAudioInput, whose destructor
	// takes the PulseAudio mainloop lock. If the destructor
	// is called inside one of the PulseAudio callbacks that
	// take copies of g.ai, the destructor will try to also
	// take the mainloop lock, causing an abort().
	ai.reset();
}

void Audio::start(const QString &input, const QString &output) {
	startInput(input);
	startOutput(output);
}

void Audio::stop() {
	// Take copies of the global AudioInput and AudioOutput
	// shared pointers to keep a reference to each of them
	// around.
	AudioInputPtr ai = g.ai;
	AudioOutputPtr ao = g.ao;

	// Reset the global AudioInput and AudioOutput shared pointers
	// to the null pointer.
	g.ao.reset();
	g.ai.reset();

	// Wait until our copies of the AudioInput and AudioOutput shared pointers
	// (ai and ao) are the only ones left.
	while ((ai.get() && ! ai.unique()) || (ao.get() && ! ao.unique())) {
		QThread::yieldCurrentThread();
	}

	// Reset our copies of the AudioInput and AudioOutput
	// shared pointers.
	// This causes the AudioInput and AudioOutput destructors
	// to be called right here in this function, on the main
	// thread. Our audio backends expect this to happen.
	//
	// One such example is PulseAudioInput, whose destructor
	// takes the PulseAudio mainloop lock. If the destructor
	// is called inside one of the PulseAudio callbacks that
	// take copies of g.ai, the destructor will try to also
	// take the mainloop lock, causing an abort().
	ai.reset();
	ao.reset();
}
