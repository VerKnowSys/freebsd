static unsigned char alaw_linear[] = {
	45, 	214, 	122, 	133, 	0, 		255, 	107, 	149, 
	86, 	171, 	126, 	129, 	0, 		255, 	117, 	138, 
	13, 	246, 	120, 	135, 	0, 		255, 	99, 	157, 
	70, 	187, 	124, 	131, 	0, 		255, 	113, 	142, 
	61, 	198, 	123, 	132,  	0, 		255, 	111, 	145, 
	94, 	163, 	127, 	128, 	0, 		255, 	119, 	136, 
	29, 	230, 	121, 	134, 	0, 		255, 	103, 	153, 
	78, 	179, 	125, 	130, 	0, 		255, 	115, 	140, 
	37, 	222, 	122, 	133, 	0, 		255, 	105, 	151, 
	82, 	175, 	126, 	129, 	0, 		255, 	116, 	139, 
	5, 	254, 	120, 	135, 	0, 		255, 	97, 	159, 
	66, 	191, 	124, 	131, 	0, 		255, 	112,	143, 
	53, 	206, 	123, 	132, 	0, 		255, 	109, 	147, 
	90, 	167, 	127, 	128, 	0, 		255,	118, 	137, 
	21, 	238, 	121, 	134, 	0, 		255, 	101,	155, 
	74, 	183, 	125, 	130, 	0, 		255, 	114, 	141, 
	49, 	210, 	123, 	133, 	0, 		255, 	108, 	148, 
	88, 	169, 	127, 	129, 	0, 		255, 	118, 	138, 
	17, 	242, 	121, 	135, 	0, 		255, 	100, 	156, 
	72, 	185, 	125, 	131, 	0, 		255, 	114, 	142, 
	64, 	194, 	124, 	132, 	0, 		255, 	112, 	144, 
	96, 	161, 	128, 	128, 	1, 		255, 	120, 	136, 
	33, 	226, 	122, 	134, 	0, 		255, 	104, 	152, 
	80, 	177, 	126, 	130, 	0, 		255, 	116, 	140, 
	41, 	218, 	122, 	133, 	0, 		255, 	106, 	150, 
	84, 	173, 	126, 	129, 	0, 		255, 	117, 	139, 
	9, 	250, 	120, 	135, 	0, 		255, 	98, 	158, 
	68, 	189, 	124, 	131, 	0, 		255, 	113, 	143, 
	57, 	202, 	123, 	132, 	0, 		255, 	110, 	146, 
	92, 	165, 	127, 	128, 	0, 		255, 	119, 	137, 
	25, 	234, 	121, 	134, 	0, 		255, 	102, 	154, 
	76, 	181, 	125, 	130, 	0, 		255, 	115, 	141, 

};

#ifndef LINEAR_ALAW_NOT_WANTED
static unsigned char linear_alaw[] = {

	252,	 172,	 172,	 172,	 172,	 80,	 80,	 80,
	80,	 208,	 208,	 208,	 208,	 16,	 16,	 16,
	16,	 144,	 144,	 144,	 144,	 112,	 112,	 112,
	112,	 240,	 240,	 240,	 240,	 48,	 48,	 48,
	48,	 176,	 176,	 176,	 176,	 64,	 64,	 64,
	64,	 192,	 192,	 192,	 192,	 0,	 0,	 0,
	0,	 128,	 128,	 128,	 128,	 96,	 96,	 96,
	96,	 224,	 224,	 224,	 224,	 32,	 32,	 32,
	160,	 160,	 88,	 88,	 216,	 216,	 24,	 24,
	152,	 152,	 120,	 120,	 248,	 248,	 56,	 56,
	184,	 184,	 72,	 72,	 200,	 200,	 8,	 8,
	136,	 136,	 104,	 104,	 232,	 232,	 40,	 40,
	168,	 86,	 214,	 22,	 150,	 118,	 246,	 54,
	182,	 70,	 198,	 6,	 134,	 102,	 230,	 38,
	166, 	 222,	 158,	 254,	 190,	 206,	 142,	 238,
	210,	 242,	 194,	 226,	 218,	 250,	 202,	 234,
	235,	 203,	 251,	 219,	 227,	 195,	 243,	 211,
	175,	 239,	 143,	 207,	 191,	 255,	 159,	 223,
	167,  	 39,	 231,	 103,	 135,	 7,	 199,	 71,
	183,	 55,	 247,	 119,	 151,	 23,	 215,	 87,
	87,	 169,	 169,	 41,	 41,	 233,	 233,	 105,
	105,	 137,	 137,	 9,	 9,	 201,	 201,	 73,
	73,	 185,	 185,	 57,	 57,	 249,	 249,	 121,
	121,	 153,	 153,	 25,	 25,	 217,	 217,	 89,
	89,	 89,	 161,	 161,	 161,	 161,	 33,	 33,
	33,	 33,	 225,	 225,	 225,	 225,	 97,	 97,
	97,	 97,	 129,	 129,	 129,	 129,	 1,	 1,
	1,	 1,	 193,	 193,	 193,	 193,	 65,	 65,
	65,	 65,	 177,	 177,	 177,	 177,	 49,	 49,
	49,	 49,	 241,	 241,	 241,	 241,	 113,	 113,
	113,	 113,	 145,	 145,	 145,	 145,	 17,	 17,
	17,	 17,	 209,	 209,	 209,	 209,	 81,	 253,
};
#endif /* !LINEAR_ALAW_NOT_WANTED */
