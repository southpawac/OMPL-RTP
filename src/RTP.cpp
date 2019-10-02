///////////////////////////////////////
// COMP/ELEC/MECH 450/550
// Project 3
// Authors: Aidan Curtis and Patrick Han
//////////////////////////////////////

#include "RTP.h"
#include <limits>
#include "ompl/base/goals/GoalSampleableRegion.h"
#include "ompl/tools/config/SelfConfig.h"


ompl::geometric::RTP::RTP(const ompl::base::SpaceInformationPtr &si) : ompl::base::Planner(si, "RTP") {
	specs_.approximateSolutions = true;
	specs_.directed = true;

	// Set the range the planner is supposed to use. (setRange())
	// This parameter greatly influences the runtime of the algorithm. 
	// It represents the maximum length of a motion to be added in the tree of motions.
	Planner::declareParam<double>("range", this, &RTP::setRange, &RTP::getRange, "0.:1.:10000.");

	// In the process of randomly selecting states in the state space to attempt to go towards, 
	// the algorithm may in fact choose the actual goal state, if it knows it, 
	// with some probability. This probability is a real number between 0.0 and 1.0; 
	// its value should usually be around 0.05 and should not be too large. 
	// It is probably a good idea to use the default value.
	Planner::declareParam<double>("goal_bias", this, &RTP::setGoalBias, &RTP::getGoalBias, "0.:.05:1.");


	// we don't need intermediate states

	// Planner::declareParam<bool>("intermediate_states", this, &RTP::setIntermediateStates, &RTP::getIntermediateStates,
	// 							"0,1");

	// addIntermediateStates_ = addIntermediateStates;
}

ompl::geometric::RTP::~RTP() {
	freeMemory();
}


void ompl::geometric::RTP::clear() {
	Planner::clear();
	sampler_.reset();
	freeMemory();
	if (nn_)
		nn_->clear();
	lastGoalMotion_ = nullptr;
}

void ompl::geometric::RTP::freeMemory()
{
	if (nn_)
	{
		std::vector<Motion *> motions;
		nn_->list(motions);
		for (auto &motion : motions)
		{
			if (motion->state != nullptr)
				si_->freeState(motion->state);
			delete motion;
		}
	}
}

ompl::base::PlannerStatus ompl::geometric::RTP::solve(const ompl::base::PlannerTerminationCondition &ptc)
{
	checkValidity();
	ompl::base::Goal *goal = pdef_->getGoal().get(); // Extract the goal state from our problem defintion (pdef pointer)
	auto *goal_s = dynamic_cast<ompl::base::GoalSampleableRegion *>(goal); // Cast to a GoalSampleableRegion

	// I think this basically builds the tree initially from input states
	while (const ompl::base::State *st = pis_.nextStart()) //pis_ : planner input states, return next valid start state
	{
		auto *motion = new Motion(si_); // create a Motion ptr, initialize Motion with state information pointer
		si_->copyState(motion->state, st); // Copy the st extracted into si_
		nn_->add(motion); // Add this motion to the tree
	}

	if (nn_->size() == 0) // If our tree is empty, that means we couldn't initialize any start states
	{
		OMPL_ERROR("%s: There are no valid initial states!", getName().c_str());
		return ompl::base::PlannerStatus::INVALID_START;
	}

	if (!sampler_)
		sampler_ = si_->allocStateSampler();

	OMPL_INFORM("%s: Starting planning with %u states already in datastructure", getName().c_str(), nn_->size());
	// Initialize both approximate and exact solution pointers
	Motion *solution = nullptr; 
	Motion *approxsol = nullptr;
	double approxdif = std::numeric_limits<double>::infinity();
	auto *rmotion = new Motion(si_);
	ompl::base::State *rstate = rmotion->state; // rstate : random state
	ompl::base::State *xstate = si_->allocState();

	while (!ptc)
	{
		/* sample random state (with goal biasing) */
		if ((goal_s != nullptr) && rng_.uniform01() < goalBias_ && goal_s->canSample())
			goal_s->sampleGoal(rstate); // With small probability, sample the goal region
		else
			sampler_->sampleUniform(rstate); // Otherwise, sample a state uniformly

		/* find closest state in the tree */
		Motion *nmotion = nn_->nearest(rmotion); // query for nmotion ("nearest motion") in the tree already
		ompl::base::State *dstate = rstate;

		/* find state to add */
		double d = si_->distance(nmotion->state, rstate); // compute the distance b.t. nearest state and random state

		// I don't think we need this
		// if (d > maxDistance_)
		// {
		// 	si_->getStateSpace()->interpolate(nmotion->state, rstate, maxDistance_ / d, xstate); // truncation step for RRT ONLY!
		// 	dstate = xstate;
		// }

		if (si_->checkMotion(nmotion->state, dstate)) // check if path between two states is valid (takes pointers to State objects)
		{
			// if (addIntermediateStates_)
			// {
			// 	std::vector<ompl::base::State *> states;
			// 	const unsigned int count = si_->getStateSpace()->validSegmentCount(nmotion->state, dstate);

			// 	if (si_->getMotionStates(nmotion->state, dstate, states, count, true, true))
			// 		si_->freeState(states[0]);

			// 	for (std::size_t i = 1; i < states.size(); ++i)
			// 	{
			// 		auto *motion = new Motion;
			// 		motion->state = states[i];
			// 		motion->parent = nmotion;
			// 		nn_->add(motion);

			// 		nmotion = motion;
			// 	}
			// }
			// else
			// {
				auto *motion = new Motion(si_); // allocate memory for a state
				si_->copyState(motion->state, dstate); // Copy random state into our new motion's state
				motion->parent = nmotion; // Set the newly copied random state's parent as our nearest motion
				nn_->add(motion); // Add the random state/motion to the tree

				nmotion = motion; // The new nearest motion is the random one we just added
			// }

			double dist = 0.0;
			bool sat = goal->isSatisfied(nmotion->state, &dist);
			if (sat) // found exact solution
			{
				approxdif = dist;
				solution = nmotion;
				break;
			}
			if (dist < approxdif) // approximate solution
			{
				approxdif = dist;
				approxsol = nmotion;
			}
		}
	}

	bool solved = false;
	bool approximate = false;
	if (solution == nullptr)
	{
		solution = approxsol;
		approximate = true;
	}

	if (solution != nullptr) // exact solution
	{
		lastGoalMotion_ = solution;

		/* construct the solution path */
		std::vector<Motion *> mpath; // mpath stores our Motion pointers for the path
		while (solution != nullptr)
		{
			mpath.push_back(solution);
			solution = solution->parent; // Keep moving up the tree of motions until nullptr (root)
		}

		/* set the solution path */
		auto path(std::make_shared<PathGeometric>(si_));
		for (int i = mpath.size() - 1; i >= 0; --i)
			path->append(mpath[i]->state);
		pdef_->addSolutionPath(path, approximate, approxdif, getName());
		solved = true;
	}

	si_->freeState(xstate);
	if (rmotion->state != nullptr)
		si_->freeState(rmotion->state);
	delete rmotion;

	OMPL_INFORM("%s: Created %u states", getName().c_str(), nn_->size());

	return {solved, approximate};
}

void ompl::geometric::RTP::setup()
{
	Planner::setup();
	tools::SelfConfig sc(si_, getName());
	sc.configurePlannerRange(maxDistance_);

	if (!nn_)
		nn_.reset(tools::SelfConfig::getDefaultNearestNeighbors<Motion *>(this));
	nn_->setDistanceFunction([this](const Motion *a, const Motion *b) { return distanceFunction(a, b); });
}


void ompl::geometric::RTP::getPlannerData(ompl::base::PlannerData &data) const
{
	Planner::getPlannerData(data);

	std::vector<Motion *> motions;
	if (nn_)
		nn_->list(motions);

	if (lastGoalMotion_ != nullptr)
		data.addGoalVertex(ompl::base::PlannerDataVertex(lastGoalMotion_->state));

	for (auto &motion : motions)
	{
		if (motion->parent == nullptr)
			data.addStartVertex(ompl::base::PlannerDataVertex(motion->state));
		else
			data.addEdge(ompl::base::PlannerDataVertex(motion->parent->state), ompl::base::PlannerDataVertex(motion->state));
	}
}

