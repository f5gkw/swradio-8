#
/*
 *    Copyright (C) 2015
 *    Jan van Katwijk (J.vanKatwijk@gmail.com)
 *    Lazy Chair Computing
 *
 *    This file is part of SDR-J.
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
 */
#
#include	"frame-processor.h"
#include	"fac-data.h"
#include	"fac-processor.h"
#include	"sdc-processor.h"
#include	"msc-processor.h"
#include	"drm-decoder.h"
#include	"equalizer-1.h"
#include	"freqsyncer.h"
#include        "word-collector.h"
#include	"referenceframe.h"
#include	"basics.h"
#include	"fac-tables.h"
#include	"viterbi-drm.h"

//	The main driving class is the frameprocessor. It is
//	implemented as running in a thread and will control:
//	a. initial synchronization and resynchronization after sync failures
//	b. getting synchronized data for the frames
//	b. assembling the FAC and SDC,
//	c. collecting Muxes and passing the Mux-es to the  MSC handler
//
//	windowDepth and qam64Roulette are just there to
//	allow some fidling - without recompilation - with the
//	size of the window used for equalization and the
//	number of iterations for the multi-level decoding
	frameProcessor::frameProcessor (drmDecoder	*mr,
	                                RingBuffer<DSPCOMPLEX> *buffer,
	                                int32_t		sampleRate,
	                                int16_t		nSymbols,
	                                int8_t		windowDepth,
                                        int8_t 		qam64Roulette) :
	                                      my_Reader (buffer, 16384, mr),
                                              viterbiDecoder (0) {
int16_t	i;
	this	-> mr 		= mr;
	this	-> buffer	= buffer;
	this	-> sampleRate	= sampleRate;
	this	-> nSymbols	= nSymbols;
	this	-> windowDepth	= windowDepth;
	this	-> qam64Roulette = qam64Roulette;
//
//	defaults, will be overruled almost immediately
	modeInf. Mode		= 1;
	modeInf. Spectrum	= 3;
	my_Equalizer		= NULL;

//	inbank and outbank have dimensions satisfying the need of the
//	most "hungry" modes. We do not know the actual mode yet
	inbank		= new DSPCOMPLEX *[symbolsperFrame (Mode_C)];
	for (i = 0; i < symbolsperFrame (Mode_C); i ++) {
	   int16_t t	= Kmax (Mode_A, modeInf. Spectrum) -
	                        Kmin (Mode_A, modeInf. Spectrum) + 1;
	   inbank [i]	= new DSPCOMPLEX [t];
	   memset (inbank [i], 0, t * sizeof (DSPCOMPLEX));
	}

	outbank		= new theSignal *[symbolsperFrame (Mode_C)];
	for (i = 0; i < symbolsperFrame (Mode_C); i ++) {
	   int16_t t	= Kmax (Mode_A, modeInf. Spectrum) -
	                        Kmin (Mode_A, modeInf. Spectrum) + 1;
	   outbank [i]	= new theSignal [t];
	   memset (outbank [i], 0, t * sizeof (theSignal));
	}

	createProcessors	(&modeInf);
	connect (this, SIGNAL (setTimeSync (bool)),
	         mr, SLOT (executeTimeSync (bool)));
	connect (this, SIGNAL (setFACSync (bool)),
	         mr, SLOT (executeFACSync (bool)));
	connect (this, SIGNAL (setSDCSync (bool)),
	         mr, SLOT (executeSDCSync (bool)));
	connect (this, SIGNAL (show_Mode (int)),
	         mr, SLOT (execute_showMode (int)));
	connect (this, SIGNAL(show_Spectrum (int)),
	         mr, SLOT (execute_showSpectrum (int)));
	connect (this, SIGNAL (show_services (int, int)),
	         mr, SLOT (show_channels (int, int)));
	connect (this, SIGNAL (show_audioMode (QString)),
	         mr, SLOT (show_audioMode (QString)));
	connect (this, SIGNAL (showSNR (float)),
	         mr, SLOT (showSNR (float)));
//
//	The driver is executed by a separate thread, therefore we
//	just set a flag when changes are to be applied
//	The thread will look regularly to see whether changes
//	are to be applied
}

	frameProcessor::~frameProcessor (void) {
int16_t	i;
	taskMayRun. store (false);
	while (isRunning ())
	   usleep (100);
	deleteProcessors	();	//	need to be addressed!!!

	for (i = 0; i < symbolsperFrame (Mode_C); i ++) {
	   delete [] inbank [i];
	   delete [] outbank [i];
	}
 	delete [] inbank;
	delete [] outbank;
}

void	frameProcessor::stop (void) {
	taskMayRun. store (false);
}

void	frameProcessor::deleteProcessors () {
	if (my_Equalizer != NULL)
	   delete	my_Equalizer;
	my_Equalizer = NULL;
	delete	my_facProcessor;
	delete	my_sdcProcessor;
	delete	my_mscProcessor;
	delete	my_facData;
	delete	my_mscConfig;
}

void	frameProcessor::createProcessors (smodeInfo *m) {
uint8_t	Mode		= m -> Mode;
uint8_t	Spectrum	= m -> Spectrum;

	if ((Spectrum > 3) ||(Spectrum <= 1)) {
	   m -> Spectrum = 3;
	   Spectrum	= 3;
	}
	

	my_mscConfig		= new mscConfig	(Mode, Spectrum);
	my_facData		= new facData	(mr, my_mscConfig);
//
	my_Equalizer 		= new equalizer_1 (Mode,
	                                           Spectrum, windowDepth);
	my_facProcessor		= new facProcessor (Mode,
	                                            Spectrum,
	                                            &viterbiDecoder);
	my_sdcProcessor		= new sdcProcessor (Mode,
	                                            Spectrum,
	                                            &viterbiDecoder,
	                                            my_facData,
	                                            sdcCells (&modeInf));
	my_mscProcessor		= new mscProcessor  (my_mscConfig,
	                                             mr,
	                                             qam64Roulette,
	                                             &viterbiDecoder);
}

//
//	The frameprocessor is the driving force. It will continuously
//	ask for ofdm symbols, build frames and send frames to the
//	various processors (performing some validity checks as part of the
//	process)
void	frameProcessor::run	(void) {
int16_t		lc;		// index of first word in inbank
uint8_t		oldMode		= 6,
		oldSpectrum	= 7;
int16_t		i;
int16_t		blockCount	= 0;
bool		inSync;
bool		superframer	= false;
int16_t		goodFrames	= 0;
int16_t		badFrames	= 0;
int16_t		threeinaRow;
int16_t		symbol_no	= 0;
bool		frameReady;
int16_t		missers;
//
	taskMayRun. store (true);	// will be set from elsewhere to false
//
//	It is cruel, but alas. Task termination is quite complex
//	in Qt (a task has to commit suicide on a notification from
//	elsewhere, We just raise an exception when the time has come ...)
	try {
restart:

	   if (!taskMayRun. load ())
	      throw (0);
	   setTimeSync		(false);
	   setFACSync		(false);
	   setSDCSync		(false);
	   show_services	(0, 0);
	   superframer		= false;	
	   goodFrames		= 0;
	   badFrames		= 0;
	   threeinaRow		= 0;
	   show_audioMode	(QString ("  "));
	   lc			= 0;
	   missers		= 0;
//
//	First step: find mode and starting point
	   modeInf. Mode = -1;
//
	   while (((modeInf. Mode == -1) ||
	          (modeInf. Mode == Mode_D)) && taskMayRun. load ()) {
	      my_Reader. shiftBuffer (Ts_of (Mode_A) / 2);
	      getMode (&my_Reader, &modeInf);
           }

	   if (!taskMayRun. load ())
	      throw (0);
	   fprintf (stderr, "found: Mode = %d, time_offset = %f, sampleoff = %f freqoff = %f\n",
	        modeInf. Mode,
	        modeInf. timeOffset,
	        modeInf. sampleRate_offset,
	        modeInf. freqOffset_fract);
//	   
//	adjust the bufferpointer:
	   my_Reader. shiftBuffer (floor (modeInf. timeOffset + 0.5));
	   int16_t time_offset_integer = floor (modeInf. timeOffset + 0.5);
//	fractional time offset:
	   float time_offset_fractional	= modeInf. timeOffset -
	                                         time_offset_integer;

//	and create a reader/processor
	   frequencySync (mr, &my_Reader, &modeInf);
           my_wordCollector     = new wordCollector (&my_Reader,
	                                             sampleRate,
                                                     modeInf. Mode,
                                                     modeInf. Spectrum, mr);

	
	   int32_t intOffset	= modeInf. freqOffset_integer;
	   setTimeSync	(true);
//
//	if the newly detected mode differs from the one we
//	were dealing with, we have to rebuild a proper
//	working environment.

	   if ((modeInf. Mode != oldMode) ||
	       (modeInf. Spectrum != oldSpectrum) ||
	        abs (intOffset) > Khz (2)) {
//	sadly, we have to delete the various processors
	      deleteProcessors ();
	      delete my_wordCollector;
//	establish a new set of workers and tables

	      createProcessors (&modeInf);
	      my_wordCollector     = new wordCollector (&my_Reader,
	                                                sampleRate,
                                                        modeInf. Mode,
	                                                modeInf. Spectrum, mr);

	      oldSpectrum	= modeInf. Spectrum;
	      oldMode		= modeInf. Mode;
	   }
	   show_Mode (modeInf. Mode); show_Spectrum (modeInf. Spectrum);
//
//	Ok, once here, we know the intoffset, we adjusted the buffer
//	for the coarse timing and we know the "time_offset_fractional"
//	OK, all set, first compute some shorthands
	   symbolsinFrame	= symbolsperFrame (modeInf. Mode);
//
//	we know that - when starting - we are not "in sync" yet
	   inSync		= false;
//
//	start here with reading ofdm words until we have a
//	match with the timesync.
//	We start with reading in "symbolsperFrame" ofdm symbols
	   for (i = 0; i < symbolsperFrame (modeInf. Mode); i ++) {
	      my_wordCollector -> getWord (inbank [i],
	                                   intOffset,
	                                   time_offset_fractional);
	      corrBank [i] = getCorr (&modeInf, inbank [i]);
	      time_offset_fractional -=  Ts_of (modeInf. Mode) *
	                                        modeInf. sampleRate_offset;
	   }
	   int16_t errors	= 0;
	   lc	= 0;
//
//	We keep on reading here until we are satisfied that the
//	frame that is in, is indeed a valid, synchronized frame

	   while (!inSync && taskMayRun. load ()) {
	      while (true) {
	         my_wordCollector -> getWord (inbank [lc],
	                                      intOffset,
	                                      time_offset_fractional);
	         time_offset_fractional -=  Ts_of (modeInf. Mode) *
	                                           modeInf. sampleRate_offset;
	         corrBank [lc] = getCorr (&modeInf, inbank [lc]);
	         lc = (lc + 1) % symbolsperFrame (modeInf. Mode);
	         if (is_bestIndex (&modeInf, lc))  {
	            break;
	         }
	      }
//
//	from here on, we know that in the input bank, the frames occupy the
//	rows "lc" ... "(lc + symbolsinFrame) % symbolsinFrame"
//	so, once here, we know that the frame starts with index lc,
//	so let us equalize the last symbolsinFrame words in the buffer
	      for (symbol_no = 0;
	           symbol_no < symbolsperFrame (modeInf. Mode); symbol_no ++) 
	         (void) my_Equalizer ->
	            equalize (inbank [(lc + symbol_no) %
	                                   symbolsperFrame (modeInf. Mode)],
	                   symbol_no, outbank);
//	The synchronizer needs some lookahead, so we know
//	that at this point there is no frame fully synchronized.
//	We are waiting for the equalizer to signal that a full
//	frame is equalized. Since we did exactly symbolsinFrame symbols,
//	symbol_no is set to 0 again
	      lc		= (lc + symbol_no) % symbol_no;
	      symbol_no		= 0;
	      frameReady	= false;
	      while (!frameReady) {
	         my_wordCollector ->
	              getWord (inbank [lc],
	                       intOffset,
	                       time_offset_fractional);
	         time_offset_fractional -=  Ts_of (modeInf. Mode) *
	                                           modeInf. sampleRate_offset,
	         corrBank [lc] = getCorr (&modeInf, inbank [lc]);
	         frameReady = my_Equalizer -> equalize (inbank [lc],
	                                                symbol_no,
	                                                outbank);
	         lc = (lc + 1) % symbolsperFrame (modeInf. Mode);
	         symbol_no = (symbol_no + 1) % symbolsperFrame (modeInf. Mode);
	      }

//	so here, we might expect that "lc" indicates the location in
//	the inputbank to store the next word, while "symbol_no" denotes
//	the next symbol in the output bank
//
//	when we are here, we do have a full "frame".
//	so, we will be convinced that we are OK when we have a decent FAC
	      if (getFACdata (&modeInf,
	                      outbank, my_facData,
	                      my_Equalizer   -> getMeanEnergy  (),
	                      my_Equalizer   -> getChannels ())) {
	         inSync = true;
	         break;		
	      }
	      else 	// if no fac, keep on trying or, start all over again
	         if (++errors > 5)
	            goto restart;
	   }

	   bool	firstTime	= true;
	   if (!taskMayRun. load ())
	      throw (21);
//
//	when here, it feels like we have passed a serious exam,
//	we have in the buffer an equalized frame (the first we met)
//	ready for further processing
//
	   float	offsetFractional	= 0;	//
	   int16_t	offsetInteger		= 0;
	   float	deltaFreqOffset		= 0;
	   float	sampleclockOffset	= 0;
	   while (taskMayRun. load ()) {
	      setFACSync (true);
//	when we are here, we can start thinking about  SDC's and superframes
//	The first frame of a superframe has an SDC part
	      if (isFirstFrame (my_facData)) {
	         bool sdcOK;
	         theSignal sdcVector [sdcCells (&modeInf)];
	         (void)extractSDC (&modeInf, outbank, sdcVector);
//	if needed create a new sdcProcessor
	         if (my_facData -> myChannelParameters. getSDCmode () !=
	                               my_sdcProcessor -> getSDCmode ()) {
	            delete my_sdcProcessor;
	            my_sdcProcessor = new sdcProcessor (modeInf. Mode,
	                                                modeInf. Spectrum, 
	                                                &viterbiDecoder,
	                                                my_facData,
	                                                sdcCells (&modeInf));
	         }
//
	         sdcOK = my_sdcProcessor -> processSDC (sdcVector); 
	         setSDCSync (sdcOK);
	         if (sdcOK) {
	            goodFrames ++;
	            threeinaRow ++;
	         }
	         else
	            badFrames ++;
	               
	         show_services (my_mscConfig -> getnrAudio (),
	                        my_mscConfig -> getnrData ());

	         if (sdcOK) 
	            my_mscProcessor -> check_mscConfig (my_mscConfig);
//
//	we allow one sdc to fail, but only after having at least
//	three frames correct
	         blockCount	= 0;
	         superframer	= sdcOK || threeinaRow >= 3;
	         if (!sdcOK)
	            threeinaRow	= 0;
	      }
//
//	when here, add the current frame to the superframe.
//	Obviously, we cannot garantee that all data is in order
	      if (superframer)
	         addtoSuperFrame (&modeInf, blockCount ++);
//
//	when we are here, it is time to build the next frame
	      frameReady	= false;
	      for (i = 0; i < symbolsperFrame (modeInf. Mode); i ++) {
	         my_wordCollector ->
	              getWord (inbank [(lc + i) %
	                               symbolsperFrame (modeInf. Mode)],
	                       intOffset,	// initial value
	                       firstTime,
	                       offsetFractional,	// tracking value
	                       deltaFreqOffset,		// tracking value
	                       sampleclockOffset	// tracking value
	                      );
	         firstTime = false;
	         corrBank [(lc + i) % symbolsperFrame (modeInf. Mode)] =
	                          getCorr (&modeInf,
	                                   inbank [(lc + i) %
	                                      symbolsperFrame (modeInf. Mode)]);
	         frameReady =
	                  my_Equalizer ->
	                         equalize (inbank [(lc + i) %
	                                      symbolsperFrame (modeInf. Mode)],
	                                   (symbol_no + i) %
	                                      symbolsperFrame (modeInf. Mode),
	                                   outbank,
	                                   &offsetInteger,
	                                   &offsetFractional,
	                                   &deltaFreqOffset,
	                                   &sampleclockOffset);
	      }

//	OK, let us check the FAC
	      if (!getFACdata (&modeInf,
	                       outbank,
	                       my_facData,
	                       my_Equalizer   -> getMeanEnergy  (),
	                       my_Equalizer   -> getChannels ())) {
	            setFACSync (false);
	            setSDCSync (false);
	            superframer		= false;
	            if (missers++ < 3)
	               continue;
	            goto restart;	// ... or give up and start all over
	      }
	      else
	         missers = 0;
	   }	// end of main loop
	}
	catch (int e) {
	   fprintf (stderr, "frameprocessor will halt with %d\n", e);
	}
}
//
//	adding the contents of a frame to a superframe
void	frameProcessor::addtoSuperFrame (smodeInfo *m, int16_t blockno) {
static	int	teller	= 0;
int16_t	symbol, carrier;
int16_t	   K_min		= Kmin (m -> Mode, m -> Spectrum);
int16_t	   K_max		= Kmax (m -> Mode, m -> Spectrum);

	if (blockno == 0)	// new superframe
	   my_mscProcessor	-> newFrame ();
	for (symbol = 0; symbol < symbolsperFrame (m -> Mode); symbol ++)
	   for (carrier = K_min; carrier <= K_max; carrier ++)
	      if (isDatacell (&modeInf, symbol, carrier, blockno)) {
	         my_mscProcessor -> addtoMux (blockno, teller ++,
	                                     outbank [symbol][carrier - K_min]);
	      }

	if (isLastFrame (my_facData)) {
	   my_mscProcessor	-> endofFrame ();
	   teller = 0;
	}
}

bool	frameProcessor::isLastFrame (facData *f) {
uint8_t	val	= f -> myChannelParameters. getIdentity ();
	return ((val & 03) == 02);
}

bool	frameProcessor::isFirstFrame (facData *f) {
uint8_t	val	= f -> myChannelParameters. getIdentity ();
	return ((val & 03) == 0) || ((val & 03) == 03);
}

bool	frameProcessor::isDatacell (smodeInfo *m,
	                            int16_t symbol,
	                            int16_t carrier, int16_t blockno) {
	if (carrier == 0)
	   return false;
	if (m -> Mode == 1 && (carrier == -1 || carrier == 1))
	   return false;
//
//	these are definitely SDC cells
	if ((blockno == 0) && ((symbol == 0) || (symbol == 1)))
	   return false;
//
	if ((blockno == 0) && ((m -> Mode == 3 ) || (m -> Mode == 4)))
	   if (symbol == 2)
	      return false;
	if (isFreqCell (m -> Mode, symbol, carrier))
	   return false;
	if (isPilotCell (m -> Mode, symbol, carrier))
	   return false;
	if (isTimeCell (m -> Mode, symbol, carrier))
	   return false;
	if (isFACcell (m, symbol, carrier))
	   return false;

	return true;
}

int16_t	frameProcessor::sdcCells (smodeInfo *m) {
int16_t	nQAM_SDC_cells	= 0;

	switch (m -> Mode) {
	   case 1:
	      switch (m -> Spectrum) {	
	         case 0:	nQAM_SDC_cells = 167; break;
	         case 1:	nQAM_SDC_cells = 190; break;
	         case 2:	nQAM_SDC_cells = 359; break;
	         case 3:	nQAM_SDC_cells = 405; break;
	         default:
	         case 4:	nQAM_SDC_cells = 754; break;
	      }
	      break;

	   default:
	   case 2:
	      switch (m -> Spectrum) {
	         case 0:	nQAM_SDC_cells	= 130; break;
	         case 1:	nQAM_SDC_cells	= 150; break;
	         case 2:	nQAM_SDC_cells	= 282; break;
	         default:
	         case 3:	nQAM_SDC_cells	= 322; break;
	         case 4:	nQAM_SDC_cells	= 588; break;
	      }
	      break;

	   case 3:
	      nQAM_SDC_cells	= 288; break;
	
	   case 4:
	      nQAM_SDC_cells	= 152; break;
	}

	return nQAM_SDC_cells;
}

int16_t	frameProcessor::mscCells  (smodeInfo *m) {
	switch (m -> Mode) {
	   case 1:
	      switch (m -> Spectrum) {
	         case	0:	return 3778;
	         case	1:	return 4368;
	         case	2:	return 7897;
	         default:
	         case	3:	return 8877;
	         case	4:	return 16394;
	      }

	   default:
	   case 2:
	      switch (m -> Spectrum) {
	         case	0:	return 2900;
	         case	1:	return 3330;
	         case	2:	return 6153;
	         default:
	         case	3:	return 7013;
	         case	4:	return 12747;
	      }

	   case 3:
	      return 5532;

	   case 4:
	      return 3679;
	}
}
//
bool	frameProcessor::is_bestIndex (smodeInfo *m, int16_t index) {
int16_t	i;

	for (i = 0; i < symbolsperFrame (m -> Mode); i ++)
	   if (corrBank [i] > corrBank [index])
	      return false;
	return true;
}

bool	frameProcessor::isFACcell (smodeInfo *m,
	                           int16_t symbol, int16_t carrier) {
int16_t	i;
struct facElement *facTable     = getFacTableforMode (m -> Mode);

//	we know that FAC cells are always in positive carriers
	if (carrier < 0)
	   return false;
	for (i = 0; facTable [i]. symbol != -1; i ++) {
	   if (facTable [i]. symbol > symbol)
	      return false;
	   if ((facTable [i]. symbol == symbol) &&
	       (facTable [i]. carrier == carrier))
	      return true;
	}
	return false;
}

bool	frameProcessor::isSDCcell (smodeInfo *m,
	                           int16_t symbol, int16_t carrier) {

	if (carrier == 0)
	   return false;
	if ((m -> Mode == 1) && ((carrier == -1) || (carrier == 1)))
	   return false;

	if (isTimeCell (m -> Mode, symbol, carrier))
	   return false;
	if (isFreqCell (m -> Mode, symbol, carrier))
	   return false;
	if (isPilotCell (m -> Mode, symbol, carrier))
	   return false;
	if (isFACcell (m, symbol, carrier))
	   return false;
	return true;
}

int16_t	frameProcessor::extractSDC (smodeInfo *m,
	                            theSignal	**bank,
	                            theSignal	*outV) {
int16_t	valueIndex	= 0;
int16_t	carrier;

	for (carrier = Kmin (m -> Mode, m -> Spectrum);
	     carrier <= Kmax (m -> Mode, m -> Spectrum); carrier ++)
	   if (isSDCcell (m, 0, carrier)) {
	      outV [valueIndex ++] = 
	           bank [0][carrier - Kmin (m -> Mode, m -> Spectrum)];
	   }

	for (carrier = Kmin (m -> Mode, m -> Spectrum);
	     carrier <= Kmax (m -> Mode, m -> Spectrum); carrier ++) 
	   if (isSDCcell (m, 1, carrier)) {
	      outV [valueIndex ++] = 
	           bank [1][carrier - Kmin (m -> Mode, m -> Spectrum)];
	   }

	if ((m -> Mode == 3) || (m -> Mode == 4)) {
	   for (carrier = Kmin (m -> Mode, m -> Spectrum);
	        carrier <= Kmax (m -> Mode, m -> Spectrum); carrier ++) 
	      if (isSDCcell (m, 2, carrier)) {
	         outV [valueIndex ++] = 
	              bank [2][carrier - Kmin (m -> Mode, m -> Spectrum)];
	      }
	}

//	fprintf (stderr, "for Mode %d we have %d sdcCells\n",
//	                   Mode, valueIndex);
	return valueIndex;
}
//
//	just for readability
uint8_t	frameProcessor::getSpectrum	(facData *f) {
uint8_t val = f -> myChannelParameters. getSpectrumBits ();
	return val <= 3 ? val : 3;
}
//
//	just a handle
float	frameProcessor::getCorr (smodeInfo *m, DSPCOMPLEX *v) {
	return timeCorrelate (v, m ->  Mode, m -> Spectrum);
}

DSPCOMPLEX frameProcessor::facError (DSPCOMPLEX *src,
	                             DSPCOMPLEX *ref,
	                             int16_t amount) {
int16_t	i;
DSPCOMPLEX ldist	= DSPCOMPLEX (0, 0);

	for (i = 0; i < amount; i ++) {
	   ldist += src [i] / ref [i];
	}
	return cdiv (ldist, amount);
}

bool	frameProcessor::getFACdata (smodeInfo *m,
	                            theSignal **bank,
	                            facData *d, float meanEnergy,
	                            DSPCOMPLEX **H) {
theSignal	facVector [100];
float		squaredNoiseSequence [100];
float		squaredWeightSequence [100];
int16_t		i;
bool		result;
static	int	count	= 0;
static	int	goodcount = 0;
float	sum_MERFAC	= 0;
float	sum_WMERFAC	= 0;
float	sum_weight_FAC	= 0;
int16_t	nFac		= 0;
float	WMERFAC;
struct facElement *facTable     = getFacTableforMode (m -> Mode);

	for (i = 0; facTable [i]. symbol != -1; i ++) {
	   int16_t symbol	= facTable [i]. symbol;
	   int16_t index	= facTable [i]. carrier -
	                                 Kmin (m -> Mode, m -> Spectrum);
	   facVector [i]	= bank [symbol][index];
	   squaredNoiseSequence [i]	= abs (facVector [i]. signalValue) - sqrt (0.5);
	   squaredNoiseSequence [i]	*= squaredNoiseSequence [i];
	   DSPCOMPLEX theH	= H [symbol][index];
	   squaredWeightSequence [i] = real (theH * conj (theH));
	   sum_MERFAC		+= squaredNoiseSequence [i];
	   sum_WMERFAC		+= squaredNoiseSequence [i] * 
	                                (squaredWeightSequence [i] + 1.0E-10);
	   sum_weight_FAC	+= squaredWeightSequence [i];
	}
	nFac		= i;
//	MERFAC		= log10 (sum_MERFAC / i + 1.0E-10);
	WMERFAC		= log10 (sum_WMERFAC /
	                         (meanEnergy * (sum_weight_FAC + nFac * 1.0E-10)));
	WMERFAC		*= -10.0;
	showSNR (WMERFAC);

//	processFAC will do two things:
//	decode the FAC samples and check the CRC and, if the CRC
//	check is OK, it will construct the FAC samples as they should have been
	result = my_facProcessor -> processFAC (facVector, d);
	if (result) {
	   goodcount ++;
	}
	if (++count == 100) {
	   printf ("%d FACs, success = %d\n", count, goodcount);
	   count = 0;
	   goodcount = 0;
	}
	return result;
}


void	frameProcessor::getMode (Reader *my_Reader, smodeInfo *m) {
timeSyncer  my_Syncer (my_Reader, sampleRate,  nSymbols);
	my_Syncer. getMode (m);
}

void    frameProcessor::frequencySync (drmDecoder *mr,
                                       Reader *my_Reader, smodeInfo *m) {
freqSyncer my_Syncer (my_Reader, m, sampleRate, mr);
           my_Syncer. frequencySync (m);
}

