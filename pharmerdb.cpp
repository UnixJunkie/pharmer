/*
Pharmer: Efficient and Exact 3D Pharmacophore Search
Copyright (C) 2011  David Ryan Koes and the University of Pittsburgh

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

/*
 * pharmerdb.cpp
 *
 *  Created on: Aug 2, 2010
 *      Author: dkoes
 */

#include "tripletmatching.h"
#include <boost/lexical_cast.hpp>
#include <fcntl.h>
#include <openbabel/mol.h>
#include "CommandLine2/CommandLine.h"
#include "Triplet.h"
#include "BitSetTree.h"
#include "ReadMCMol.h"
#include "Timer.h"
#include "pharmerdb.h"
#include "TripleIndexer.h"
#include "PMol.h"

#ifdef OUTPUTSMINA
#include "SminaConverter.h"
#endif

using namespace boost;
using namespace std;
using namespace OpenBabel;

extern cl::opt<bool> Quiet;
extern cl::opt<unsigned> ReduceConfs;
extern cl::opt<bool> ComputeThresholds;

//location comparison functions for pointdata
bool comparePointDataX(const ThreePointData& lhs, const ThreePointData& rhs)
{
	return lhs.l1 < rhs.l1;
}

//location comparison functions for pointdata
bool comparePointDataY(const ThreePointData& lhs, const ThreePointData& rhs)
{
	return lhs.l2 < rhs.l2;
}

//location comparison functions for pointdata
bool comparePointDataZ(const ThreePointData& lhs, const ThreePointData& rhs)
{
	return lhs.l3 < rhs.l3;
}

//write out to buf, return number of bytes written
unsigned MolDataCreator::ConfCreator::write(unsigned char *buf)
{
	unsigned pos = 0;
	memcpy(buf, &header, sizeof(header));
	pos += sizeof(header);

	memcpy(buf + pos, molData.data(), molData.size());
	pos += molData.size();

	return pos;
}

//initialize data for a conformation
MolDataCreator::ConfCreator::ConfCreator(unsigned mid, float mw, unsigned cid,
		OBMol& m) :
	header(mid, mw, cid)
{
	PMolCreator mol(m, true);
	stringstream mdata;
	if(mol.writeBinary(mdata))
		molData = mdata.str();
	else
		molData = "";
}

unsigned MolDataCreator::maxIndex = 0;

const char *rotBondsSMART =
		"[!$([NH]!@C(=O))&!D1&!$(*#*)]-&!@[!$([NH]!@C(=O))&!D1&!$(*#*)]";

//generate pharma points and database infor for mol
void MolDataCreator::processMol(OBMol& mol, double mWeight, unsigned mid)
{
	//identify pharma
	vector<vector<PharmaPoint> > mcpoints;
	getPharmaPointsMC(pharmas, mol, mcpoints);
	double weight = mol.GetMolWt();

	//count number of rotatable bounds
	OBSmartsPattern rot;
	rot.Init(rotBondsSMART);
	vector<vector<int> > maplist;
	rot.Match(mol, maplist, OBSmartsPattern::AllUnique);
	unsigned numrot = maplist.size();

	//store each conformation seperately, this results in about 50% more space used,
	//but as much as 50% performance improvement when retrieving mols

	//generate information for each conformation
	for (unsigned c = 0, nc = mol.NumConformers(); c < nc; c++)
	{
		unsigned confidx = c;
		mol.SetConformer(confidx);
		confs.push_back(ConfCreator(mid, mWeight, confidx, mol));
		ConfCreator& conf = confs.back();

		if(!conf.isValid())
		{
			cerr << mol.GetTitle() << " is too large.  Omitting.\n";
			confs.pop_back();
			continue;
		}
		//setup pharma points
		const vector<PharmaPoint>& points = mcpoints[confidx];

		if (maxIndex < points.size())
			maxIndex = points.size();

		//compute and canonicalize all triple points
		unsigned n = points.size();
		if (n > (1 << TPD_INDEX_BITS))
		{
			cerr << "Too many pharma points (" << n << ") in "
					<< mol.GetTitle() << "\n";
			n = (1 << TPD_INDEX_BITS);
		}

		for (unsigned i = 0; i < n; i++)
		{
			for (unsigned j = i + 1; j < n; j++)
			{
				for (unsigned k = j + 1; k < n; k++)
				{
					unsigned pos = tindex(points[i].pharma->index,
							points[j].pharma->index, points[k].pharma->index);
					assert(pos < pdatas.size());
					ThreePointData trip(bufferSize, weight, numrot, points, i, j, k);
					pdatas[pos].push_back(trip);
				}
			}
		}

		confOffsets.push_back(bufferSize);
		bufferSize += conf.byteSize();
		numConfs++;
	}
}

//flatten all the conf info into a buffer
void MolDataCreator::createBuffer()
{
	buffer = new unsigned char[bufferSize];
	unsigned pos = 0;
	for (unsigned i = 0, n = confs.size(); i < n; i++)
	{
		pos += confs[i].write(&buffer[pos]);
	}
	assert(pos == bufferSize);

	//clear mem
	confs.clear();
}

//write out the data with proper offsets to files (assumed positioned properly and single threaded)
unsigned MolDataCreator::write(FILE *molData, vector<PointDataFile >& pointDataFiles)
{
	unsigned long location = ftell(molData);

	location = ftell(molData);

	unsigned mid = location >> (TPD_MOLDATA_BITS - TPD_MOLID_BITS);

	//update the point datas

	for (unsigned p = 0, np = pdatas.size(); p < np; p++)
	{
		for (unsigned i = 0, n = pdatas[p].size(); i < n; i++)
		{
			pdatas[p][i].molPos += location;
		}
	}


	//write out
	fwrite(buffer, bufferSize, 1, molData);
	for (unsigned p = 0, np = pdatas.size(); p < np; p++)
	{
		pointDataFiles[p].write(&pdatas[p][0], sizeof(pdatas[p][0]),
				pdatas[p].size());
	}

	//align the start of every mol so the upper MOLID_BITS correspond to an mol id
	//(assuming the conformers fit within MOLDATA-MOLID bits)
	//we assume (hope) the filesystem has support for holes
	location = ftell(molData);
	location >>= (TPD_MOLDATA_BITS - TPD_MOLID_BITS);
	location++;
	location <<= (TPD_MOLDATA_BITS - TPD_MOLID_BITS);
	fseek(molData, location, SEEK_SET);

	if (ftell(molData) >= (1L << TPD_MOLDATA_BITS))
	{
		cerr
				<< "Input database is too large (moldata more than 1TB). Split and retry.\n";
		abort();
	}
	return mid;
}

void MolData::clear()
{
	mol = NULL;
	confidx = 0;
	mid = 0;
	molWeight = 0;
}

bool MolData::read(FILE *molData, PMolReader& areader)
{
	clear();
	MolDataHeader header;
	//get fixed sized header
	int ret = fread(&header, sizeof(header), 1, molData);
	assert(ret == 1);

	//copy to self
	confidx = header.confidx;
	molWeight = header.molWeight;
	mid = header.molID;

	//now get mol itself
	mol = areader.readPMol(molData);

	return mol != NULL;
}

//read in mol data at position location from file molData
bool MolData::read(FILE *molData, unsigned long location, PMolReader& areader)
{
	fseek(molData, location, SEEK_SET);
	return read(molData, areader);
}

//read in mol data at position location in array molData
bool MolData::read(unsigned char *molData, unsigned long location,
		PMolReader& areader)
{
	clear();
	MolDataHeader header;
	//get fixed sized header
	unsigned long loc = location;
	memcpy(&header, &molData[loc], sizeof(header));
	loc += sizeof(header);

	//copy to self
	confidx = header.confidx;
	molWeight = header.molWeight;
	mid = header.molID;

	//now get mol itself
	mol = areader.readPMol(&molData[loc]);

	return mol != NULL;
}

//read just metadata, don't parse full mol
void MolData::readDataOnly(unsigned char *molData, unsigned long location)
{
	clear();
	MolDataHeader header;
	//get fixed sized header
	unsigned long loc = location;
	memcpy(&header, &molData[loc], sizeof(header));
	loc += sizeof(header);

	//copy to self
	confidx = header.confidx;
	molWeight = header.molWeight;
	mid = header.molID;
}

void PharmerDatabaseCreator::initializeDatabases()
{
	//initialize self-managed flat file databases
	//open for writing
	//info
	boost::filesystem::path ipath = dbpath;
	ipath /= "info";
	assert(!boost::filesystem::exists(ipath));
	info = fopen(ipath.string().c_str(), "w+"); //create
	assert(info);

	//human readable pharmacophore classes
	boost::filesystem::path lpath = dbpath;
	lpath /= "lookup";
	lookup = fopen(lpath.string().c_str(), "w"); //create
	assert(lookup);

	//mid index (to better support lots of conformers)
	boost::filesystem::path mpath = dbpath;
	mpath /= "mids";
	midList = fopen(mpath.string().c_str(), "w"); //create
	assert(midList);

	//moldata
	boost::filesystem::path mdpath = dbpath;
	mdpath /= "molData";
	molData = fopen(mdpath.string().c_str(), "w+");
	assert(molData);

	//smina data
#ifdef OUTPUTSMINA
	boost::filesystem::path smipath = dbpath / "sminaIndex";
	sminaIndex = fopen(smipath.string().c_str(), "w+");
	assert(sminaIndex);
	boost::filesystem::path smdpath = dbpath / "sminaData";
	sminaData = fopen(smdpath.string().c_str(), "w+");
	assert(sminaData);
#endif

	//bincnts
	boost::filesystem::path binpath = dbpath;
	binpath /= "binCnts";
	binData = fopen(binpath.string().c_str(), "w+");
	assert(binData);

	//output pharmas
	boost::filesystem::path phpath = dbpath / "pharmas";
	ofstream pharmout(phpath.string().c_str());
	pharmas.write(pharmout);

	//pointData
	pointDataFiles.resize(tindex.size());
	for (int i = 0, n = tindex.size(); i < n; i++)
	{
		string pname = string("pointData_") + lexical_cast<string> (i);
		boost::filesystem::path pdpath = dbpath / pname;
		pointDataFiles[i] = PointDataFile(pdpath.string(), i);
	}

	//geoData
	geoDataFiles.resize(tindex.size(), NULL);

}


//write out index into "correct" mid
void PharmerDatabaseCreator::writeMIDs()
{
	if(midList)
	{
		fseek(midList, 0, SEEK_SET);
		unsigned cnt = 0;
		for(unsigned i = 0, n = mids.size(); i < n; i++)
		{
			fwrite(&mids[i],sizeof(unsigned),1,midList);
			cnt++;
			while(i+1 < n && cnt < mids[i+1])
			{
				fwrite(&mids[i],sizeof(unsigned),1,midList);
				cnt++;
			}
		}
	}
}

void PharmerDatabaseCreator::writeStats()
{
	if (info)
	{
		fseek(info, 0, SEEK_SET);
		fwrite(&stats, sizeof(stats), 1, info);
		fflush(info);
	}
	if (lookup)
	{
		tindex.dump(lookup);
	}
	writeMIDs();
}

//add a multiconformer mol to the database; weighted is passed in to assure
//weight used for segregration matches reported weight
void PharmerDatabaseCreator::addMolToDatabase(OBMol& mol, double weight)
{
	static OBAromaticTyper aromatics;
	static OBAtomTyper atyper;

	if(mol.NumAtoms() == 0) //skipped
		return;

	mol.Center();
	//ARG!!! these routines cruelly remove conformer data so we need
	//a separate OBMol for each conformer so we can add hydrogens
	// I wish this were more efficient and less fragile..
	unsigned nc = mol.NumConformers();
	if (ReduceConfs > 0 && ReduceConfs < nc)
		nc = ReduceConfs;
	vector<OBMol> confs(nc);
	for (unsigned c = 0; c < nc; c++)
	{
		confs[c] = mol;
		confs[c].SetConformer(c);
		confs[c].AddHydrogens();
	}
	mol.AddHydrogens();
	//all the implicitly called perception routines are absolutely not
	//thread safe, so do all perception here (hopefully haven't missed anything)
	mol.FindRingAtomsAndBonds();
	mol.FindChiralCenters();
	mol.PerceiveBondOrders();
	aromatics.AssignAromaticFlags(mol);
	mol.FindSSSR();
	atyper.AssignTypes(mol);
	atyper.AssignHyb(mol);

	//now add back conformer coordinates, create new memory for mol
	vector<double*> cdata(confs.size());
	unsigned ncoords = mol.NumAtoms() * 3;
	for (unsigned i = 0, n = confs.size(); i < n; i++)
	{
		assert(confs[i].NumAtoms()*3 == ncoords);
		cdata[i] = new double[ncoords];
		memcpy(cdata[i], confs[i].GetConformer(0), ncoords*sizeof(double));
	}

	mol.SetConformers(cdata);

	//generate moldata
	MolDataCreator mdc(pharmas, tindex, mol, weight, stats[NumMols]);
	unsigned mid = mdc.write(molData, pointDataFiles);
	mids.push_back(mid);

	//output smina data here
#ifdef OUTPUTSMINA
	SminaConverter::MCMolConverter mcsmina(mol);
	const vector<unsigned>& confOffsets = mdc.ConfOffsets();
	unsigned long mloc = mid;
	mloc <<= (TPD_MOLDATA_BITS - TPD_MOLID_BITS);
	for(unsigned i = 0, n = confOffsets.size(); i < n; i++)
	{
		stringstream data;
		mcsmina.convertConformer(i, data);
		unsigned sz = data.str().size();
		unsigned long pos = mloc + confOffsets[i]; //location in molData
		unsigned long smpos = ftell(sminaData); //location in smina dta

		//output index mapping
		fwrite(&pos,sizeof(pos),1,sminaIndex);
		fwrite(&smpos,sizeof(smpos),1,sminaIndex);

		//output actual data
		fwrite(&sz, sizeof(sz),1, sminaData);
		fwrite(data.str().c_str(), sizeof(char), sz, sminaData);
	}
#endif

	stats[NumMols]++;
	stats[NumConfs] += mdc.NumConfs();
	stats[NumDbPoints] += mdc.NumPoints();
}
/*
 * Add the molecules in infile from start offset by stride to the database.
 * Sequential molecules with the same name must be conformers of the same mol.
 *
 * This is single threaded since bottlenecks in openbabel eliminate any advantages to multithreading
 */
void PharmerDatabaseCreator::addMolsToDatabase(istream& infile,
		OBFormat *format, unsigned start, unsigned stride,
		unsigned long readBytes, unsigned long lastByte)
{
	//add all the molecules
	Timer t;

	OBMol mol;
	ReadMCMol reader(infile, format, stride, start, ReduceConfs);

	while (reader.read(mol))
	{
		addMolToDatabase(mol, mol.GetMolWt());
	}

	if(ComputeThresholds)
		thresholdComputer.printBestThresholds();
	writeStats();

	if (!Quiet)
	{
		cout << "\nAdded in " << t.elapsed() << "s (" << t.elapsedProcess()
				<< "s)\n";
		cout << "\t" << stats[NumMols] << "\tmolecules\t";
		unsigned long location = ftell(molData);
		location >>= (TPD_MOLDATA_BITS - TPD_MOLID_BITS);
		cout << "(" << location << " logical)\n";
		cout << "\t" << stats[NumConfs] << "\tconformations\n";
		cout << "\t" << stats[NumDbPoints] << "\tpoints\n";
		cout << "\t" << MolDataCreator::MaxIndex() << "\tmaxIndex\n";
	}
}

//coalesce very small collections and
//mmap pointData files, closing file pointer
void PharmerDatabaseCreator::initPointDataArrays()
{
	assert(pointDataArrays == NULL);
	unsigned n = pointDataFiles.size();
	pointDataArrays = new MMappedRegion<ThreePointData> [n];
	for (unsigned i = 0; i < n; i++)
	{
		if(pointDataFiles[i].isValid())
		{
			pointDataFiles[i].close();
			pointDataArrays[i].map(pointDataFiles[i].name, false, true);
		}
	}
}

//split along the the longest access
//fill in split info
static void chooseSplit(const BoundingBox& box, SplitInfo& info,
		unsigned long long numPoints, unsigned long long numUnique,
		unsigned pos)
{

	if (numPoints == 0)
	{
		info.type = NoSplit;
	}
	else if ((numUnique <= MAX_UNIQUE_POINTS_PER_LEAF || numPoints
			< MAX_POINTDATAS_PER_LEAF))
	{
		//not worth further dividing
		info.type = NoSplit;
	}
	else if (2 * pos >= SPLITS_PER_GEOPAGE && numPoints
			< MIN_POINTDATAS_PER_PAGE)
	{
		info.type = NoSplit;
		//don't create a whole new geo page
	}
	else //do a split
	{
		int xdiff = box.maxx - box.minx;
		int ydiff = box.maxy - box.miny;
		int zdiff = box.maxz - box.minz;

		if (xdiff >= ydiff && xdiff >= zdiff)
		{
			info.type = SplitXFixed;
			info.min = box.minx;
			info.max = box.maxx;
			info.func = comparePointDataX;
		}
		else if (ydiff >= xdiff && ydiff >= zdiff)
		{
			info.type = SplitYFixed;
			info.min = box.miny;
			info.max = box.maxy;
			info.func = comparePointDataY;
		}
		else //z
		{
			info.type = SplitZFixed;
			info.min = box.minz;
			info.max = box.maxz;
			info.func = comparePointDataZ;
		}
	}
}

class PivotCompareRnd
{
	short pivot;
	const SplitInfo& info;
public:
	PivotCompareRnd(short p, const SplitInfo& i) :
		pivot(p), info(i)
	{
	}

	//flip a coint as to which way equal vals go
	bool operator()(const ThreePointData& x)
	{
		int diff = info.splitVal(x) - pivot;
		if (diff < 0)
			return true;
		if (diff > 0)
			return false;
		return ::random() % 2;
	}
};

//find the median value in one sequential scan using counts
static unsigned short findMedianValue(ThreePointData *start, ThreePointData *end,
		const SplitInfo& info, unsigned short min, unsigned short max)
{
	unsigned long total = 0;
	int left = min;
	int right = left + (max - min);
	unsigned long counts[right - left + 1];
	memset(counts, 0, sizeof(counts));

	for (ThreePointData *itr = start; itr != end; itr++)
	{
		counts[info.splitVal(*itr) - left]++;
		total++;
	}
	unsigned long mid = total / 2;
	unsigned long cnt = 0;
	for (int i = left; i <= right; i++)
	{
		cnt += counts[i - left];
		if (cnt >= mid)
			return i;
	}
	abort();
}

//create an internal kd tree
//each node splits points data in two along the axis with the largest spread
//every node has an explicit bounding box (so in that sense this is more like an R-tree)
void PharmerDatabaseCreator::doSplitInPage(unsigned pharma, FILE *geoFile, GeoKDPage& page,
		unsigned pos, ThreePointData *start, ThreePointData *end, ThreePointData *begin,
		unsigned depth)
{
	//first compute the bounding box and count points
	unsigned long numPoints = end - start;
	unsigned long numUnique = 0;
	TripleInt last(0, 0, 0);
	BoundingBox box;
	for (ThreePointData *itr = start; itr != end; itr++)
	{
		TripleInt coords(itr->l1, itr->l2, itr->l3);
		box.update(coords);
		if (coords != last)
		{
			last = coords;
			numUnique++;
		}
	}

	//choose splitting dimension
	SplitInfo info;
	chooseSplit(box, info, numPoints, numUnique, pos);

	page.nodes[pos].box = box;
	page.nodes[pos].splitType = info.type;
	if (info.type == NoSplit)
	{
		//this is the end
	}
	else //need to split
	{
		//choose a median value, points with this value can be on either side
		unsigned short medianVal = findMedianValue(start, end, info, info.getMin(box),
				info.getMax(box));
		PivotCompareRnd rcmp(medianVal, info);
		ThreePointData *median = partition(start, end, rcmp);

		if (median == end || median == start) //we got unlikely and did not partition well
		{
			sort(start, end, info.func);
			median = start + (end - start) / 2;
		}

		page.nodes[pos].splitVal = medianVal;
		page.nodes[pos].splitData = median - begin;

		if (2 * pos + 1 < SPLITS_PER_GEOPAGE) //stay on internal page
		{
			if ((end - start) < pdatasFitInMemory && tcnt.threadAvailable())
			{
				thread t(thread_start_doSplitInPage, this, pharma, geoFile, boost::ref(
						page), 2 * pos, start, median, begin, depth);
				doSplitInPage(pharma, geoFile, page, 2 * pos + 1, median, end, begin, depth);
				t.join();
				tcnt.reduceThreadCount();
			}
			else
			{
				doSplitInPage(pharma, geoFile, page, 2 * pos, start, median, begin, depth);
				doSplitInPage(pharma, geoFile, page, 2 * pos + 1, median, end, begin, depth);
			}
		}
		else //children pushed to new page
		{
			assert(2*pos >= SPLITS_PER_GEOPAGE);
			unsigned lpos = 2 * pos - SPLITS_PER_GEOPAGE;
			unsigned rpos = lpos + 1;
			assert(rpos < SPLITS_PER_GEOPAGE);
			page.nextPages[lpos] = doSplitNewPage(pharma, geoFile, start, median, begin, depth);
			page.nextPages[rpos] = doSplitNewPage(pharma, geoFile, median, end, begin, depth);
		}
	}
}

void PharmerDatabaseCreator::thread_start_doSplitInPage(
		PharmerDatabaseCreator *pharmdb, unsigned pharma, FILE *geoFile, GeoKDPage& page,
		unsigned pos, ThreePointData *start, ThreePointData *end, ThreePointData *begin,
		unsigned depth)
{
	pharmdb->doSplitInPage(pharma, geoFile, page, pos, start, end, begin, depth);
}

//create a geokdpage that indexes the data between start and end in geoFile
//writes out page to geoFile and returns its location as an index into an array
//of geokdpages
unsigned long PharmerDatabaseCreator::doSplitNewPage(unsigned pharma, FILE *geoFile,
		ThreePointData *start, ThreePointData *end, ThreePointData *begin, unsigned depth)
{
	GeoKDPage page;

	//reserve space
	unique_lock<shared_mutex> lock(fileAccessLock);
	fseek(geoFile, 0, SEEK_END);
	unsigned long location = ftell(geoFile);
	assert(location % sizeof(GeoKDPage) == 0);
	int ret = fwrite(&page, sizeof(page), 1, geoFile);
	assert(ret == 1);
	lock.unlock();

	depth++;
	//create the page
	doSplitInPage(pharma, geoFile, page, 1, start, end, begin, depth);

	stats[NumInternalPages]++;

	//write it out
	lock.lock();
	fseek(geoFile, location, SEEK_SET);
	location /= sizeof(GeoKDPage); //indexify
	ret = fwrite(&page, sizeof(page), 1, geoFile);
	assert(ret == 1);

	return location;
}

//saturated binning
static unsigned lengthbin(unsigned len)
{
	unsigned ret = len / LENGTHDIV;
	if(ret >= LENGTH_BINS)
		ret = LENGTH_BINS-1;

	return ret;
}

void PharmerDatabaseCreator::incrementBinCnt(const ThreePointData& t, unsigned pclass)
{
	binnedCnts[pclass][lengthbin(t.l1)][lengthbin(t.l2)][lengthbin(t.l3)]++;
}

//create spatial index for all ijk triples
void PharmerDatabaseCreator::createIJKSpatialIndex(int p)
{
	//create spatial
	ThreePointData *start = pointDataArrays[p].begin();
	ThreePointData *begin = start;
	ThreePointData *end = pointDataArrays[p].end();

	if(start == end)
		return;

	//count binned frequencies
	for (ThreePointData *itr = start; itr != end; itr++)
	{
		incrementBinCnt(*itr, p);
	}

	//open
	string gname = string("geoData_") + lexical_cast<string> (p);
	boost::filesystem::path gpath = dbpath / gname;
	FILE * geoFile= fopen(gpath.string().c_str(), "w");
	geoDataFiles[p] = geoFile;
	assert(geoFile);

	//write out blank page, there's nothing at index 0
	fseek(geoFile, 0, SEEK_SET);
	GeoKDPage blank;
	fwrite(&blank, sizeof(blank), 1, geoFile);

	//top-down recursively create kd/r tree spatial data structure
	doSplitNewPage(p, geoFile, start, end, begin, 0);
}

/* Create spatial index. */
void PharmerDatabaseCreator::createSpatialIndex()
{
	Timer t;
	cout << "Creating spatial index...\n";
	//close and then mmap pointinfo file
	initPointDataArrays();

	for (unsigned p = 0, n = tindex.size(); p < n; p++)
		createIJKSpatialIndex(p);

	for(unsigned i = 0, n = binnedCnts.size(); i < n; i ++)
		fwrite(binnedCnts[i].c_array(),  LENGTH_BINS*LENGTH_BINS*LENGTH_BINS, sizeof(unsigned), binData);

	cout << stats[NumConfs] << "\tconformations\n";
	cout << stats[NumMols] << "\tmolecules\n";
	cout << stats[NumInternalPages] << "\tinternal pages\n";
	cout << "Index creation time: " << t.elapsed() << "s ("
			<< t.elapsedProcess() << "s)" << "\n";

	writeStats();

}

//Searcher


void PharmerDatabaseSearcher::initializeDatabases()
{
	//initialize self-managed flat file databases
	//open for reading
	//info - and read it in
	boost::filesystem::path ipath = dbpath;
	ipath /= "info";
	info = fopen(ipath.string().c_str(), "r");
	assert(info);
	int read = fread(&stats, sizeof(stats), 1, info);
	if (read != 1)
	{
		cerr << ipath.string() << "\n";
		abort();
	}

	//pharmas
	boost::filesystem::path pharmpath = dbpath / "pharmas";
	ifstream pharmain(pharmpath.string().c_str());
	if (!pharmas.read(pharmain))
	{
		cerr << "Invalid pharma specification in database.\n";
		return;
	}

	tindex.set(pharmas.size());

	//moldata
	boost::filesystem::path mdpath = dbpath;
	mdpath /= "molData";
	molData.map(mdpath.string(), true, true);

	//smina index and data - do not need to exist
	boost::filesystem::path smIndex = dbpath / "sminaIndex";
	if(boost::filesystem::exists(smIndex))
	{
		sminaIndex.map(smIndex.string(), true, true);
		boost::filesystem::path smData = dbpath / "sminaData";
		sminaData.map(smData.string(), true, true);
	}

	//mids
	boost::filesystem::path mpath = dbpath;
	mpath /= "mids";
	if(boost::filesystem::exists(mpath)) //back-wards compat
		midList.map(mpath.string(), true, true,true);

	//length histogram
	boost::filesystem::path binpath = dbpath / "binCnts";
	binnedCnts.map(binpath.string(),true,false);
	if(binnedCnts.length() == 0)
	{
		cerr << "Missing binnedCnts " << binpath << "\n";
		return;
	}
	//pointData
	unsigned n = tindex.size();
	tripletDataArrays = new MMappedRegion<ThreePointData> [n];
	for (unsigned i = 0; i < n; i++)
	{
		string pname = string("pointData_") + lexical_cast<string> (i);
		boost::filesystem::path pdpath = dbpath / pname;
		if(boost::filesystem::exists(pdpath))
			tripletDataArrays[i].map(pdpath.string(), true, true);
	}

	//geoData
	geoDataArrays = new MMappedRegion<GeoKDPage> [n];
	for (unsigned i = 0; i < n; i++)
	{
		string gname = string("geoData_") + lexical_cast<string> (i);
		boost::filesystem::path gpath = dbpath / gname;
		if(boost::filesystem::exists(gpath))
			geoDataArrays[i].map(gpath.string(), true, true);
	}

	valid = true;
}

unsigned PharmerDatabaseSearcher::getBinCnt(unsigned pclass, unsigned i, unsigned j, unsigned k)
{
	unsigned index = pclass;
	index *= LENGTH_BINS;
	index += i;
	index *= LENGTH_BINS;
	index += j;
	index *= LENGTH_BINS;
	index += k;

	return binnedCnts[index];
}

//generate ranking and return index of best triplet
//use binnedCnts histogram
unsigned PharmerDatabaseSearcher::rankTriplets(const
		vector<QueryTriplet>& triplets, vector<double>& ranking)
{
	double minval = HUGE_VAL;
	unsigned besttrip = 0;
	ranking.resize(triplets.size(), 0);

	//add up values from histogram
	for (unsigned i = 0, n = triplets.size(); i < n; i++)
	{
		double val = 0;
		const QueryTriplet& trip = triplets[i];
		unsigned pclass = tindex(trip.getPharma(0), trip.getPharma(1),
				trip.getPharma(2));

		for (unsigned x = lengthbin(trip.getRanges()[0].min), mx =
				lengthbin(trip.getRanges()[0].max); x <= mx; x++)
			for (unsigned y = lengthbin(trip.getRanges()[1].min), my =
					lengthbin(trip.getRanges()[1].max); y <= my; y++)
				for (unsigned z = lengthbin(trip.getRanges()[2].min),
						mz = lengthbin(trip.getRanges()[2].max); z
						<= mz; z++)
				{
					val += getBinCnt(pclass,x,y,z);
				}

		ranking[i] = val * triplets[i].numOrderings(); //symmetries will increases matches

		if (val < minval)
		{
			minval = val;
			besttrip = i;
		}
	}
	return besttrip;
}

//get values along appropriate axis for split
static void getMinMax(const Triplet& t, SplitType split, int& min, int& max)
{
	min = max = 0;
	switch (split)
	{
	case SplitXFixed:
		min = t.getRanges()[0].min;
		max = t.getRanges()[0].max;
		break;
	case SplitYFixed:
		min = t.getRanges()[1].min;
		max = t.getRanges()[1].max;
		break;
	case SplitZFixed:
		min = t.getRanges()[2].min;
		max = t.getRanges()[2].max;
		break;
	default:
		;
	}
}

inline void dump(ostream& out, const ThreePointData& tpd)
{
	out << tpd.molPos << ": " << tpd.l1 <<" " << tpd.l2 << " " << tpd.l3 << "\n";
}

void PharmerDatabaseSearcher::queryProcessPoints(QueryInfo& t,
		unsigned long startLoc, unsigned long endLoc)
{
	const ThreePointData *start = t.data + startLoc;
	const ThreePointData *end = t.data + endLoc;

	//put the range of point datas into the priorty queue,
	//let the PQ filter out bad values
	unsigned cnt = 0;
	for (const ThreePointData *itr = start; itr != end; itr++)
	{
		unsigned mid = getBaseMID(itr->molID());
//cout << " Match "; dump(cout, *itr);
		if (t.M.add(mid, *itr, t.triplet, t.which))
		{
			cnt++;
		}
	}
	if (cnt == 0)
		emptyCnt++;
	matchedCnt += cnt;
	processCnt += (end - start);
}

cl::opt<unsigned> searchCutoff("search-cutoff", cl::desc(
		"Number of triplets to stop kd-tree splitting at"), cl::Hidden,
		cl::init(2048));

static unsigned pageCnt = 0;
void PharmerDatabaseSearcher::queryIndex(QueryInfo& t, const GeoKDPage *page,
		unsigned pos, unsigned long startLoc, unsigned long endLoc)
{
	if(startLoc == endLoc)
		return;
	if (pos >= SPLITS_PER_GEOPAGE)
	{
		if (t.stopEarly)
			return;
		//need another page
		unsigned long index = page->nextPages[pos - SPLITS_PER_GEOPAGE];
		pageCnt++;
		queryIndex(t, &t.pages[index], 1, startLoc, endLoc);
	}
	else
	{
		//internal to this page
		const GeoKDPageNode& node = page->nodes[pos];
		//are there potentially points within this region?
		if (!t.triplet.inRange(node.box))
		{
			return;
		}
		if (node.splitType == NoSplit)
		{
			queryProcessPoints(t, startLoc, endLoc);
		}
		else if (endLoc - startLoc < searchCutoff)
		{
			//don't spend a lot of time at the edges, just read and process
			queryProcessPoints(t, startLoc, endLoc);
		}
		else if (t.triplet.allInRange(node.box))
		{
			//this worth doing both when accessing disk and, surprisingly,
			//when accessing memory
			//all points beneath are valid (maybe)
			queryProcessPoints(t, startLoc, endLoc);
		}
		else //consider splitter
		{
			int min, max;
			getMinMax(t.triplet, node.splitType, min, max);

			//some sanity checks
			assert(startLoc <= node.splitData);
			assert(endLoc >= node.splitData);

			//always do left and then right so we access data sequentially
			//if min or max are exactly equal, then we must consider both sides of the split
			//in case the split is in the middle of a range of equal values
			if ((max > node.splitVal && min < node.splitVal) || max == node.splitVal || min == node.splitVal)
			{
				//have to do both sides
				//first go left
				queryIndex(t, page, 2 * pos, startLoc, node.splitData);
				queryIndex(t, page, 2 * pos + 1, node.splitData, endLoc);
			}
			else if (max <= node.splitVal) //just the left
			{
				queryIndex(t, page, 2 * pos, startLoc, node.splitData);
			}
			else if (min >= node.splitVal) //just right
			{
				queryIndex(t, page, 2 * pos + 1, node.splitData, endLoc);
			}
		}
	}
}

//put all matching triplets into Q
//each triplet is expanded to get all overlapping trips
//this is intentionally not multi-threaded - performance-wise it's better to
//split the database up and parralel match
void PharmerDatabaseSearcher::generateTripletMatches(const vector<vector<
		QueryTriplet> >& triplets, TripletMatches& M, bool& stopEarly)
{
	pageCnt = 0;
	emptyCnt = 0;
	processCnt = 0;
	matchedCnt = 0;
	for (unsigned i = 0, n = triplets.size(); i < n; i++)
	{
		for (unsigned t = 0, nt = triplets[i].size(); t < nt; t++)
		{
			const QueryTriplet& trip = triplets[i][t];
//cout << "Searching "; trip.dump(); cout << "\n";
			unsigned pclass = tindex(trip.getPharma(0), trip.getPharma(1),
					trip.getPharma(2));
			const GeoKDPage *pages = geoDataArrays[pclass].begin();
			const ThreePointData *data = tripletDataArrays[pclass].begin();
			QueryInfo qinfo(trip, t, pages, data, i, M, stopEarly);
			queryIndex(qinfo, &pages[1], 1, 0,
					tripletDataArrays[pclass].length());
		}

		if (!Quiet)
		{
			cout << i << " MatchedCnt " << matchedCnt << " ProcessedCnt "
					<< processCnt << "  " << 100.0 * double(matchedCnt)
					/ processCnt << "% ";
            cout << "Pages " << pageCnt << " ";
            cout << "Empty " << emptyCnt << " ";
			cout << "Chunks: " << M.numChunks() << "\n";
		}
		if (!M.nextIndex())
			break;

		if (stopEarly)
			break;
	}
	if (!Quiet)
	{
		cout << "PageCnt :" << pageCnt << "\n";
		cout << "EmptyCnt : " << emptyCnt << "\n";
		cout << "MatchedCnt : " << matchedCnt << "\n";
		cout << "ProcessedCnt : " << processCnt << "\n";
	}
}

typedef pair<unsigned long, unsigned long> ulong_pair;
static bool first_pair_cmp(const ulong_pair& lhs, const ulong_pair& rhs)
{
	return lhs.first < rhs.first;
}
//find the smina data location given the moldata location and copy the smina
//data to out
void PharmerDatabaseSearcher::getSminaData(unsigned long molloc, ostream& out)
{
	ulong_pair findit(molloc, 0);

	ulong_pair *pos = lower_bound(sminaIndex.begin(), sminaIndex.end(), findit, first_pair_cmp);

	//molloca may be a little larger than the index molloc (different anchor positions)
	if(pos == sminaIndex.end())
	{
		pos = sminaIndex.end()-1;
	}
	else if(pos->first > molloc)
	{
		pos--;
	}
	assert(pos >= sminaIndex.begin());

	//pos is now the correct spot
	unsigned long sminaloc = pos->second;
	unsigned sz = 0;
	memcpy(&sz, sminaData.begin()+sminaloc, sizeof(unsigned)); //size of smina data
	out.write(sminaData.begin()+sminaloc+sizeof(unsigned), sz);
}

