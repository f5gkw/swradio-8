#
/*
 *    Copyright (C)  2018
 *    Jan van Katwijk (J.vanKatwijk@gmail.com)
 *    Lazy Chair Programming
 *
 *    This file is part of SDR-J (JSDR).
 *
 *    SDR-J is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    SDR-J is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with SDR-J; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
#include	<unistd.h>
#include	<QSettings>
#include	<QDebug>
#include	<QDateTime>
#include	<QObject>
#include	<QDir>
#include	<QMessageBox>
#include	<QFileDialog>
#include	"radio.h"
#include	"fft-scope.h"
#include	"upconverter.h"
#include        "audiosink.h"
#include        "decimator.h"
#include        "shifter.h"
#include        "popup-keypad.h"
#include        "program-list.h"
//
//      devices
#include        "virtual-input.h"
#include        "filereader.h"
#ifdef  HAVE_SDRPLAY
#include        "sdrplay-handler.h"
#endif
//
//	decoders
#include	"virtual-decoder.h"
#ifdef	HAVE_AM_DECODER
#include	"am-decoder.h"
#endif
#ifdef	HAVE_SSB_DECODER
#include	"ssb-decoder.h"
#endif
#ifdef	HAVE_CW_DECODER
#include	"cw-decoder.h"
#endif
#ifdef	HAVE_AMTOR_DECODER
#include	"amtor-decoder.h"
#endif
#ifdef	HAVE_PSK_DECODER
#include	"psk-decoder.h"
#endif
#ifdef	HAVE_RTTY_DECODER
#include	"rtty-decoder.h"
#endif
#ifdef	HAVE_FAX_DECODER
#include	"fax-decoder.h"
#endif
#ifdef	HAVE_DRM_DECODER
#include	"drm-decoder.h"
#endif

	RadioInterface::RadioInterface (QSettings	*sI,
	                                QString		stationList,
	                                int		inputRate,
	                                int		decoderRate,
	                                QWidget		*parent):
	                                    QMainWindow (parent),
	                                    hfShifter   (inputRate),
	                                    hfFilter    (1024, 377),
	                                    lfFilter    (1024, 127),
	                                    agc         (decoderRate, 12),
	                                    theDecimator (inputRate,
	                                                       decoderRate) {

	this	-> settings	= sI;
	this	-> inputRate	= inputRate;
	this	-> decoderRate	= decoderRate;
	setupUi (this);
//      and some buffers
//	in comes:
        inputData       = new RingBuffer<std::complex<float> > (1024 * 1024);
//
//	out goes:
	audioRate		= 48000;
        audioData       = new RingBuffer<std::complex<float>> (audioRate);

	agc. setMode (agcHandler::AGC_OFF);

//	setup the devices
	deviceTable	-> addItem ("filereader");
#ifdef	HAVE_SDRPLAY
	deviceTable	-> addItem ("sdrplay");
#endif
//
//	and the decoders
	theDecoder	= new virtualDecoder (decoderRate,
	                                      audioData);

#ifdef	HAVE_AM_DECODER
	decoderTable	-> addItem ("am decoder");
#endif
#ifdef	HAVE_SSB_DECODER
	decoderTable	-> addItem ("ssb decoder");
#endif
#ifdef	HAVE_CW_DECODER
	decoderTable	-> addItem ("cw decoder");
#endif
#ifdef	HAVE_AMTOR_DECODER
	decoderTable	-> addItem ("amtor decoder");
#endif
#ifdef	HAVE_PSK_DECODER
	decoderTable	-> addItem ("psk decoder");
#endif
#ifdef	HAVE_RTTY_DECODER
	decoderTable	-> addItem ("rtty decoder");
#endif
#ifdef	HAVE_FAX_DECODER
	decoderTable	-> addItem ("wfax decoder");
#endif
#ifdef	HAVE_DRM_DECODER
	decoderTable	-> addItem ("drm decoder");
#endif
	displaySize		= 1024;
	scopeWidth		= inputRate;
	theBand. currentOffset	= scopeWidth / 4;
	theBand. lowF		= -decoderRate / 2;
	theBand. highF		= decoderRate / 2;
	mouseIncrement		= 5;
//	scope and settings
	hfScopeSlider	-> setValue (50);
        hfScope		= new fftScope (hfSpectrum,
                                        displaySize,
                                        kHz (1),        // scale
                                        inputRate,
                                        hfScopeSlider -> value (),
                                        8);
	hfScope		-> set_bitDepth (12);	// default
	hfScope	-> setScope (14070 * 1000, scopeWidth / 4);
        connect (hfScope,
                 SIGNAL (clickedwithLeft (int)),
                 this,
                 SLOT (adjustFrequency_khz (int)));
	connect (hfScope,
	         SIGNAL (clickedwithRight (int)),
	         this, SLOT (switch_hfViewMode (int)));
        connect (hfScopeSlider, SIGNAL (valueChanged (int)),
                 this, SLOT (set_hfscopeLevel (int)));

//      scope and settings
	lfScopeSlider	-> setValue (50);
        lfScope		= new fftScope (lfSpectrum,
                                        displaySize,
	                                1,
                                        decoderRate,
                                        lfScopeSlider -> value (),
                                        8);
	lfScope		-> set_bitDepth (12);
        connect (lfScope,
                 SIGNAL (clickedwithLeft (int)),
                 this,
                 SLOT (adjustFrequency_hz (int)));
	connect (lfScope,
	         SIGNAL (clickedwithRight (int)),
	         this, SLOT (switch_lfViewMode (int)));
        connect (lfScopeSlider, SIGNAL (valueChanged (int)),
                 this, SLOT (set_lfscopeLevel (int)));

//	output device
        audioHandler            = new audioSink (this -> audioRate, 16384);
        outTable                = new int16_t
                                     [audioHandler -> numberofDevices () + 1];
        for (int i = 0; i < audioHandler -> numberofDevices (); i ++)
           outTable [i] = -1;

        try {
           setupSoundOut (streamOutSelector, audioHandler,
                          audioRate, outTable);
        } catch (int e) {
           QMessageBox::warning (this, tr ("sdr"),
                                       tr ("Opening audio failed\n"));
           abort ();
	}
	audioHandler -> selectDefaultDevice ();

        connect (deviceTable, SIGNAL (activated (const QString &)),
                 this, SLOT (setStart (const QString &)));
        connect (decoderTable, SIGNAL (activated (const QString &)),
                 this, SLOT (selectDecoder (const QString &)));

        mykeyPad                = new keyPad (this);
        connect (freqButton, SIGNAL (clicked (void)),
                 this, SLOT (handle_freqButton (void)));

	connect (middleButton, SIGNAL (clicked (void)),
	         this, SLOT (set_inMiddle (void)));

	connect (bandSelector, SIGNAL (activated (const QString &)),
	         this, SLOT (setBand (const QString &)));

	connect (quitButton, SIGNAL (clicked (void)),
                 this, SLOT (handle_quitButton (void)));

	connect (mouse_Inc, SIGNAL (valueChanged (int)),
	         this, SLOT (set_mouseIncrement (int)));

        connect (streamOutSelector, SIGNAL (activated (int)),
                 this, SLOT (setStreamOutSelector (int)));

	connect (AGC_select, SIGNAL (activated(const QString&) ),
	         this, SLOT (set_AGCMode (const QString&) ) );

	connect (agc_thresholdSlider, SIGNAL (valueChanged (int)),
	         this, SLOT (set_agcThresholdSlider (int)));

	connect (freqSave, SIGNAL (clicked (void)),
                 this, SLOT (set_freqSave (void)));

        myList  = new programList (this, stationList);
        myList  -> show ();
	myLine	= NULL;
        audioHandler    -> selectDefaultDevice ();
	theUpConverter	= new upConverter (decoderRate, 48000, audioHandler);

	theDevice		= NULL;
	delayCount		= 0;
	dumpfilePointer		= NULL;

	connect (dumpButton, SIGNAL (clicked (void)),
	         this, SLOT (set_dumpButton (void)));
	secondsTimer. setInterval (1000);
	connect (&secondsTimer, SIGNAL (timeout (void)),
                 this, SLOT (updateTime (void)));
        secondsTimer. start (1000);
}

//      The end of all
        RadioInterface::~RadioInterface () {
	secondsTimer. stop ();
        delete  mykeyPad;
        delete  myList;
	delete	theUpConverter;
}
//
//	If the user quits before selecting a device ....
void	RadioInterface::handle_quitButton	(void) {
	if (theDevice != NULL) {
	   theDevice	-> stopReader ();
	   disconnect (theDevice, SIGNAL (dataAvailable (int)),
	               this, SLOT (sampleHandler (int)));
	   delete  theDevice;
	}
	myList          -> saveTable ();
	myList		-> hide ();
	delete	theDecoder;
	close ();
}

//	Note that to simplify things, we select a device once
//	at program start
void	RadioInterface::setStart	(const QString &s) {
	try {
	   theDevice	= selectDevice (s, inputData);
	} catch (int e) {
	   return;
	}

	deviceTable	-> hide ();
	connect (theDevice, SIGNAL (dataAvailable (int)),
	         this, SLOT (sampleHandler (int)));
//
//	and off we go
	theDevice	-> restartReader ();
}

virtualInput	*RadioInterface::selectDevice (const QString &s,
	                                       RingBuffer<std::complex<float>> *hfBuffer) {

virtualInput *res;
	if (s == "filereader")
	   res	= new fileReader (this, inputRate, hfBuffer, settings);
#ifdef	HAVE_SDRPLAY
	else
	   res  = new sdrplayHandler (this, inputRate, hfBuffer, settings);
#endif
	agc. set_bitDepth (res -> bitDepth ());
	agc_thresholdSlider -> setMinimum (get_db (0, res -> bitDepth ()));
        agc_thresholdSlider -> setMaximum (0);
        agc_thresholdSlider -> setValue (get_db (0, res -> bitDepth ()));
	return res;
}
//
//	
virtualDecoder	*RadioInterface::selectDecoder (const QString &s) {
	disconnect (theDecoder, SIGNAL (audioAvailable (int, int)),
	            this, SLOT (processAudio (int, int)));
	disconnect (theDecoder, SIGNAL (setDetectorMarker (int)),
	            this, SLOT (setDetectorMarker (int)));
	delete theDecoder;
#ifdef	HAVE_AM_DECODER
	if (s == "am decoder") {
	   theDecoder	= new amDecoder (decoderRate,
	                                  audioData, settings);
	}
	else
#endif
#ifdef	HAVE_SSB_DECODER
	if (s == "ssb decoder") {
	   theDecoder	= new ssbDecoder (decoderRate,
	                                   audioData, settings);
	}
	else
#endif
#ifdef	HAVE_CW_DECODER
	if (s == "cw decoder") {
	   theDecoder	= new cwDecoder (decoderRate,
	                                  audioData, settings);
	}
	else
#endif
#ifdef	HAVE_AMTOR_DECODER
	if (s == "amtor decoder") {
	   theDecoder	= new amtorDecoder (decoderRate,
	                                     audioData, settings);
	}
	else
#endif
#ifdef	HAVE_PSK_DECODER
	if (s == "psk decoder") {
	   theDecoder	= new pskDecoder (decoderRate,
	                                   audioData, settings);
	}
	else
#endif
#ifdef	HAVE_RTTY_DECODER
	if (s == "rtty decoder") {
	   theDecoder	= new rttyDecoder (decoderRate,
	                                    audioData, settings);
	}
	else
#endif
#ifdef	HAVE_FAX_DECODER
	if (s == "wfax decoder") {
	   theDecoder	= new faxDecoder (decoderRate,
	                                   audioData, settings);
	}
	else
#endif
#ifdef	HAVE_DRM_DECODER
	if (s == "drm decoder") {
	   theDecoder	= new drmDecoder (decoderRate,
	                                   audioData, settings);
	}
	else
#endif
	   theDecoder	= new virtualDecoder (decoderRate, audioData);
//	bind the signals in the decoder to some slots
	connect (theDecoder, SIGNAL (audioAvailable (int, int)),
	         this, SLOT (processAudio (int, int)));
	connect (theDecoder, SIGNAL (setDetectorMarker (int)),
	         this, SLOT (setDetectorMarker (int)));
	return theDecoder;
}
//
void    RadioInterface::handle_freqButton (void) {
        if (mykeyPad -> isVisible ())
           mykeyPad -> hidePad ();
        else
           mykeyPad     -> showPad ();
}

//	setFrequency is called from the frequency panel
//	as well as from inside to change VFO and offset
void	RadioInterface::setFrequency (int frequency) {
int	VFOFrequency	= frequency - scopeWidth / 4;
	theDevice	-> setVFOFrequency (VFOFrequency);
	theBand. currentOffset	= scopeWidth / 4;
	hfScope		-> setScope  (VFOFrequency, theBand. currentOffset);
	hfFilter. setBand (theBand. currentOffset + theBand. lowF,
	                   theBand. currentOffset + theBand. highF,
	                                          inputRate);
	frequencyDisplay	-> display (frequency);
}
//
//	adjustFrequency is called whenever clicking the mouse
void	RadioInterface::adjustFrequency_khz (int32_t n) {
	adjustFrequency_hz (1000 * n);
}

void	RadioInterface::adjustFrequency_hz (int32_t n) {
int	lowF	= theBand. lowF;
int	highF	= theBand. highF;
int	currOff	= theBand. currentOffset;

	if ((currOff + highF + n >= scopeWidth / 2 - scopeWidth / 20) ||
	    (currOff + lowF + n <= - scopeWidth / 2 + scopeWidth / 20) ) {
	   int32_t newFreq = theDevice -> getVFOFrequency () +
	                                   theBand. currentOffset + n;
	   setFrequency (newFreq);
	   return;
	}
	else {
	   theBand. currentOffset += n;
	   hfScope -> setScope (theDevice -> getVFOFrequency (),
	                                   theBand. currentOffset);
	   hfFilter. setBand (theBand. currentOffset + theBand. lowF,
	                      theBand. currentOffset + theBand. highF,
	                            inputRate);
	}

	frequencyDisplay	-> display (theDevice -> getVFOFrequency () +
	                                    theBand. currentOffset);
}
//
//	just a convenience button
void	RadioInterface::set_inMiddle (void) {
	int32_t newFreq = theDevice -> getVFOFrequency () +
	                                    theBand. currentOffset;
	setFrequency (newFreq);
}

void    RadioInterface::set_freqSave    (void) {
        if (myLine == NULL)
           myLine  = new QLineEdit ();
        myLine  -> show ();
        connect (myLine, SIGNAL (returnPressed (void)),
                 this, SLOT (handle_myLine (void)));
}

void    RadioInterface::handle_myLine (void) {
int32_t freq    = theDevice -> getVFOFrequency () + theBand. currentOffset;
QString programName     = myLine -> text ();
        myList  -> addRow (programName, QString::number (freq / Khz (1)));
        disconnect (myLine, SIGNAL (returnPressed (void)),
                    this, SLOT (handle_myLine (void)));
        myLine  -> hide ();
}

void	RadioInterface::setBand	(const QString &s) {
	if (s == "am") {
	   theBand. lowF	= -4500;
	   theBand. highF	= 4500;
	}
	else
	if (s == "usb") {
	   theBand. lowF	= 0;
	   theBand. highF	= 2500;
	}
	else
	if (s == "lsb") {
	   theBand. lowF	= -2500;
	   theBand. highF	= 0;
	}
	else
	if (s == "wide") {
	   theBand. lowF	= -5500;
	   theBand. highF	= 5500;
	}
	hfFilter. setBand (theBand. currentOffset + theBand. lowF,
	                   theBand. currentOffset + theBand. highF,
	                                                inputRate);
}

void	RadioInterface::set_mouseIncrement (int inc) {
	mouseIncrement = inc;
}

void	RadioInterface::wheelEvent (QWheelEvent *e) {
	adjustFrequency_hz ((e -> delta () > 0) ?
	                        mouseIncrement : -mouseIncrement);
}

//////////////////////////////////////////////////////////////////
//
void	RadioInterface::sampleHandler (int amount) {
std::complex<float>   buffer [theDecimator. inSize ()];
std::complex<float> ifBuffer [theDecimator. outSize ()];
float dumpBuffer [2 * 512];
int	i, j;

	while (inputData -> GetRingBufferReadAvailable () > 512) {
	   inputData	-> getDataFromBuffer (buffer, 512);
	   hfScope	-> addElements (buffer, 512);
	   if (dumpfilePointer != NULL) {
	      for (i = 0; i < 512; i ++) {
                 dumpBuffer [2 * i]	= real (buffer [i]);
                 dumpBuffer [2 * i + 1] = imag (buffer [i]);
              }
              sf_writef_float (dumpfilePointer, dumpBuffer, 512);
           }
	      
	   for (i = 0; i < 512; i ++) {
	      float agcGain;
	      std::complex<float> temp = hfFilter. Pass (buffer [i]);
	      temp = hfShifter. do_shift (temp, theBand. currentOffset);
	      if (theDecimator. add (temp, ifBuffer)) {
	         lfScope -> addElements (ifBuffer, theDecimator. outSize ());
	         for (j = 0; j < theDecimator. outSize (); j ++) {
	            agcGain	= agc. doAgc (ifBuffer [j]);
	            temp	= cmul (ifBuffer [j], agcGain);
	            theDecoder -> process (temp);
	         }
	      }
	   }
	}
}
//
//
void    RadioInterface::set_hfscopeLevel (int level) {
        hfScope -> setLevel (level);
}

void    RadioInterface::set_lfscopeLevel (int level) {
        lfScope -> setLevel (level);
}

//	do not forget that ocnt starts with 1, due
//	to Qt list conventions
void	RadioInterface::setupSoundOut (QComboBox	*streamOutSelector,
	                               audioSink	*our_audioSink,
	                               int32_t		cardRate,
	                               int16_t		*table) {
uint16_t	ocnt	= 1;
uint16_t	i;

	for (i = 0; i < our_audioSink -> numberofDevices (); i ++) {
	   const char *so =
	             our_audioSink -> outputChannelwithRate (i, cardRate);

	   if (so != NULL) {
	      streamOutSelector -> insertItem (ocnt, so, QVariant (i));
	      table [ocnt] = i;
	      ocnt ++;
	   }
	}

	qDebug () << "added items to combobox";
	if (ocnt == 1)
	   throw (22);
}

void	RadioInterface::setStreamOutSelector (int idx) {
int16_t	outputDevice;

	if (idx == 0)
	   return;

	outputDevice = outTable [idx];
	if (!audioHandler -> isValidDevice (outputDevice)) 
	   return;

	audioHandler	-> stop	();
	if (!audioHandler -> selectDevice (outputDevice)) {
	   QMessageBox::warning (this, tr ("sdr"),
	                               tr ("Selecting  output stream failed\n"));
	   return;
	}

	qWarning () << "selected output device " << idx << outputDevice;
	audioHandler	-> restart ();
}

void    RadioInterface::processAudio  (int amount, int rate) {
DSPCOMPLEX buffer [rate / 10];

        (void)amount;
	usleep (1000);
        while (audioData -> GetRingBufferReadAvailable () >
                                                 (uint32_t)rate / 10) {
           audioData -> getDataFromBuffer (buffer, rate / 10);
	   if (rate != 48000)
	      theUpConverter    -> handle (buffer, rate / 10);
	   else
	      audioHandler      -> putSamples (buffer, rate / 10);
        }
}

void    RadioInterface::set_AGCMode (const QString &s) {
uint8_t gainControl;

        if (s == "off AGC")
           gainControl  = agcHandler::AGC_OFF;
        else
        if (s == "slow")
           gainControl  = agcHandler::AGC_SLOW;
        else
           gainControl  = agcHandler::AGC_FAST;
        agc. setMode      (gainControl);
}

void    RadioInterface::set_agcThresholdSlider (int v) {
        agc. setThreshold (v);
}

void	RadioInterface::setDetectorMarker	(int m) {
	lfScope -> setScope  (0, m);
}

void	RadioInterface::switch_hfViewMode	(int d) {
	(void)d;
	hfScope	-> switch_viewMode ();
}

void	RadioInterface::switch_lfViewMode	(int d) {
	(void)d;
	lfScope	-> switch_viewMode ();
}

void	RadioInterface::updateTime		(void) {
QDateTime currentTime = QDateTime::currentDateTime ();

	timeDisplay     -> setText (currentTime.
                                    toString (QString ("dd.MM.yy:hh:mm:ss")));
}

void	RadioInterface::set_dumpButton (void) {
SF_INFO	*sf_info	= (SF_INFO *)alloca (sizeof (SF_INFO));

	if (dumpfilePointer != NULL) {
	   sf_close (dumpfilePointer);
	   dumpfilePointer	= NULL;
	   dumpButton	-> setText ("dump");
	   return;
	}

	QString file = QFileDialog::getSaveFileName (this,
	                                        tr ("Save file ..."),
	                                        QDir::homePath (),
	                                        tr ("PCM wave file (*.wav)"));
	if (file == QString (""))
	   return;
	file		= QDir::toNativeSeparators (file);
	if (!file.endsWith (".wav", Qt::CaseInsensitive))
	   file.append (".wav");
	sf_info		-> samplerate	= inputRate;
	sf_info		-> channels	= 2;
	sf_info		-> format	= SF_FORMAT_WAV | SF_FORMAT_FLOAT;

	dumpfilePointer	= sf_open (file. toUtf8 (). data (),
	                                   SFM_WRITE, sf_info);
	if (dumpfilePointer == NULL) {
	   qDebug () << "Cannot open " << file. toUtf8 (). data ();
	   return;
	}

	dumpButton		-> setText ("WRITING");
}
