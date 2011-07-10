/*
 * ut_VolumeFraction.cxx
 *
 *  Created on: Jun 20, 2011
 *      Author: jamitch
 */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_ALTERNATIVE_INIT_API
#include <boost/test/unit_test.hpp>
#include <boost/test/parameterized_test.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <string>
#include "mesh_input/quick_grid/QuickGrid.h"
#include "mesh_input/quick_grid/QuickGridData.h"
#include "quick_grid/calculators.h"
#include "pdneigh/NeighborhoodList.h"
#include "pdneigh/PdZoltan.h"
#include "utilities/PdutMpiFixture.h"
#include "mesh_output/vtk/Field.h"
#include "mesh_output/vtk/PdVTK.h"
#include "utilities/Vector.h"
#include "utilities/Array.h"
#include <set>

#include "Epetra_Comm.h"
#include "Epetra_BlockMap.h"
#include "Epetra_Vector.h"
#include "Epetra_Import.h"

#include "Epetra_ConfigDefs.h"
#ifdef HAVE_MPI
#include "mpi.h"
#include "Epetra_MpiComm.h"
#else
#include "Epetra_SerialComm.h"
#endif




#include <tr1/memory>
#include <iostream>
#include <cmath>
using namespace Pdut;



using namespace QUICKGRID;
using UTILITIES::Vector3D;
using UTILITIES::Array;
using Field_NS::Field;
using Field_NS::FieldSpec;
using std::tr1::shared_ptr;
using namespace boost::unit_test;
using std::size_t;
using std::string;
using std::cout;

static size_t myRank;
static size_t numProcs;
const string json_filename="./input_files/ut_CartesianTensorProductVolumeFraction.json";

void compute_neighborhood_volumes
(
		const PDNEIGH::NeighborhoodList& list,
		Field<double>& neighborhoodVol,
		Field<double>& naiveNeighborhoodVol,
		Array<double>& overlapCellVol,
		shared_ptr<double> xOverlap,
		const VOLUME_FRACTION::VolumeFractionCalculator& calculator
);

void compute_cell_volumes
(
		const PDNEIGH::NeighborhoodList& list,
		Field<double>& specialCellVolume,
		shared_ptr<double> xOverlapPtr,
		const VOLUME_FRACTION::VolumeFractionCalculator& calculator
);

void cube()
{

    // Create an empty property tree object
    using boost::property_tree::ptree;
    ptree pt;

    // Load the json file into the property tree. If reading fails
    // (cannot open file, parse error), an exception is thrown.
    read_json(json_filename, pt);

    /*
     * Get Discretization
     */
    ptree discretization_tree=pt.find("Discretization")->second;
    string path=discretization_tree.get<string>("Type");
    double horizon=pt.get<double>("Discretization.Horizon");

	double xStart = pt.get<double>(path+".X Origin");
	BOOST_CHECK(0.0==xStart);
	double yStart = pt.get<double>(path+".Y Origin");
	BOOST_CHECK(0.0==yStart);
	double zStart = pt.get<double>(path+".Z Origin");
	BOOST_CHECK(0.0==zStart);

	double xLength = pt.get<double>(path+".X Length");
	BOOST_CHECK(1.0==xLength);
	double yLength = pt.get<double>(path+".Y Length");
	BOOST_CHECK(1.0==yLength);
	double zLength = pt.get<double>(path+".Z Length");
	BOOST_CHECK(1.0==zLength);


	const int nx = pt.get<int>(path+".Number Points X");
	BOOST_CHECK(10==nx);
	const int ny = pt.get<int>(path+".Number Points Y");
	BOOST_CHECK(10==ny);
	const int nz = pt.get<int>(path+".Number Points Z");
	BOOST_CHECK(10==nz);
	const QUICKGRID::Spec1D xSpec(nx,xStart,xLength);
	const QUICKGRID::Spec1D ySpec(ny,yStart,yLength);
	const QUICKGRID::Spec1D zSpec(nz,zStart,zLength);

	VOLUME_FRACTION::VolumeFractionCalculator calculator(xSpec,ySpec,zSpec,horizon);

	// Create decomposition iterator
	QUICKGRID::TensorProduct3DMeshGenerator cellPerProcIter(numProcs, horizon, xSpec, ySpec, zSpec);
	QUICKGRID::QuickGridData gridData = QUICKGRID::getDiscretization(myRank, cellPerProcIter);;


	// This load-balances
	gridData = PDNEIGH::getLoadBalancedDiscretization(gridData);

	/*
	 * Create neighborhood with an enlarged horizon
	 */
	double cell_diagonal=calculator.get_cell_diagonal();
	shared_ptr<Epetra_Comm> comm(new Epetra_MpiComm(MPI_COMM_WORLD));
	PDNEIGH::NeighborhoodList list(comm,gridData.zoltanPtr.get(),gridData.numPoints,gridData.myGlobalIDs,gridData.myX,horizon+cell_diagonal);
	Array<double> xOverlapArray;
	Array<double> vOverlapArray;
	{
		/*
		 * mesh coordinates overlap
		 */
		Epetra_BlockMap ownedMap(*list.getOwnedMap(3));
		Epetra_BlockMap overlapMap(*list.getOverlapMap(3));
		xOverlapArray = Array<double>(overlapMap.NumMyElements()*3);
		Epetra_Import importNDF(overlapMap,ownedMap);
		Epetra_Vector xOverlap(View,overlapMap,xOverlapArray.get());
		Epetra_Vector xOwned(View,ownedMap,list.get_owned_x().get());
		xOverlap.Import(xOwned,importNDF,Insert);
	}

	{
		/*
		 * volume overlap
		 */
		Epetra_BlockMap ownedMap(*list.getOwnedMap(1));
		Epetra_BlockMap overlapMap(*list.getOverlapMap(1));
		vOverlapArray = Array<double>(overlapMap.NumMyElements()*1);
		vOverlapArray.set(0.0);
		Epetra_Import importNDF(overlapMap,ownedMap);
		Epetra_Vector vOverlap(View,overlapMap,vOverlapArray.get());
		Epetra_Vector vOwned(View,ownedMap,gridData.cellVolume.get());
		vOverlap.Import(vOwned,importNDF,Insert);
	}





	/*
	 * Compute volume on neighborhood for every point in mesh;
	 * Points with a spherical neighborhood that are completely enclosed
	 * should have a volume that very closely matches the analytical value for a sphere
	 */
	FieldSpec neighVolSpec
	(Field_ENUM::TYPE_UNDEFINED,Field_ENUM::POINT,Field_ENUM::SCALAR, Field_ENUM::CONSTANT,"neighVol");

	FieldSpec naiveNeighVolSpec
	(Field_ENUM::TYPE_UNDEFINED,Field_ENUM::POINT,Field_ENUM::SCALAR,Field_ENUM::CONSTANT,"naiveNeighVol");

	FieldSpec quadratureCellVolSpec
	(Field_ENUM::TYPE_UNDEFINED,Field_ENUM::POINT,Field_ENUM::SCALAR,Field_ENUM::CONSTANT,"quadratureCellVol");

	Field<double> neighVol(neighVolSpec,list.get_num_owned_points());
	Field<double> naiveNeighVol(naiveNeighVolSpec,list.get_num_owned_points());
	Field<double> quadratureCellVol(quadratureCellVolSpec,list.get_num_owned_points());
	Field<double> cellVol(Field_NS::VOLUME,gridData.cellVolume,list.get_num_owned_points());
	compute_cell_volumes(list,quadratureCellVol,xOverlapArray.get_shared_ptr(),calculator);
	compute_neighborhood_volumes(list,neighVol,naiveNeighVol,vOverlapArray,xOverlapArray.get_shared_ptr(),calculator);

	/*
	 * Output mesh
	 */
	vtkSmartPointer<vtkUnstructuredGrid> grid = PdVTK::getGrid(gridData.myX,gridData.numPoints);
	Field<int> fieldRank(Field_NS::PROC_NUM,gridData.numPoints);
	fieldRank.set(myRank);
	PdVTK::writeField(grid,fieldRank);
	PdVTK::writeField(grid,neighVol);
	PdVTK::writeField(grid,naiveNeighVol);
	PdVTK::writeField(grid,quadratureCellVol);
	PdVTK::writeField(grid,cellVol);
	vtkSmartPointer<vtkXMLPUnstructuredGridWriter> writer = PdVTK::getWriter("ut_VolumeFraction.pvtu", numProcs, myRank);
	PdVTK::write(writer,grid);

}


void compute_neighborhood_volumes
(
		const PDNEIGH::NeighborhoodList& list,
		Field<double>& neighborhoodVol,
		Field<double>& naiveNeighborhoodVol,
		Array<double>& overlapCellVol,
		shared_ptr<double> xOverlapPtr,
		const VOLUME_FRACTION::VolumeFractionCalculator& calculator
)
{
	size_t N = list.get_num_owned_points();
	BOOST_CHECK(neighborhoodVol.get_num_points()==N);
	BOOST_CHECK(naiveNeighborhoodVol.get_num_points()==N);

	const int *neighPtr = list.get_local_neighborhood().get();
	const double *xOwned = list.get_owned_x().get();
	const double *xOverlap = xOverlapPtr.get();
	const double *cellVolOverlap = overlapCellVol.get();
	double *neighVol = neighborhoodVol.get();
	double *naiveNeighVol = naiveNeighborhoodVol.get();
	for(int p=0;p<N;p++, xOwned +=3, neighVol++,naiveNeighVol++){
		int numNeigh = *neighPtr; neighPtr++;
		/*
		 * initialize neighborhood to zero;
		 * computes contributions from neighborhood and does not
		 * include self volume
		 */
		*neighVol = 0.0;
		*naiveNeighVol = 0.0;

		const double *P = xOwned;

		/*
		 * Loop over neighborhood of point P and compute
		 * fractional volume
		 */
		for(int n=0;n<numNeigh;n++,neighPtr++){
			int localId = *neighPtr;
			const double *Q = &xOverlap[3*localId];
			double cellVolume = cellVolOverlap[localId];
			*neighVol += calculator(P,Q);
			*naiveNeighVol += cellVolume;
		}
	}
}

void compute_cell_volumes
(
		const PDNEIGH::NeighborhoodList& list,
		Field<double>& specialCellVolume,
		shared_ptr<double> xOverlapPtr,
		const VOLUME_FRACTION::VolumeFractionCalculator& calculator
)
{
	size_t N = list.get_num_owned_points();
	BOOST_CHECK(specialCellVolume.get_num_points()==N);

	const double *xOwned = list.get_owned_x().get();
	const double *xOverlap = xOverlapPtr.get();
	double *vOwned = specialCellVolume.get();
	for(int p=0;p<N;p++, xOwned +=3, vOwned++){

		const double *P = xOwned;

		/*
		 * compute cell volume using quick grid quadrature
		 */
		*vOwned=calculator.cellVolume(P);

	}
}


bool init_unit_test_suite()
{
	// Add a suite for each processor in the test
	bool success=true;

	test_suite* proc = BOOST_TEST_SUITE( "ut_VolumeFraction" );
	proc->add(BOOST_TEST_CASE( &cube ));
	framework::master_test_suite().add( proc );

	return success;

}

bool init_unit_test()
{
	init_unit_test_suite();
	return true;
}

int main
(
		int argc,
		char* argv[]
)
{
	// Initialize MPI and timer
	PdutMpiFixture myMpi = PdutMpiFixture(argc,argv);

	// These are static (file scope) variables
	myRank = myMpi.rank;
	numProcs = myMpi.numProcs;

	// Initialize UTF
	return unit_test_main( init_unit_test, argc, argv );
}
