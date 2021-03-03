#include <memory>
#include <vector>
#include <cstdlib>
#include <iostream>
#include <stack>
#include <map>
#include <sstream>
#include <utility>
#include <set>
#include <cmath>
using std::vector;
using std::map;
using std::pair;

#include "FWCore/Framework/interface/Frameworkfwd.h"
#include "FWCore/Framework/interface/stream/EDProducer.h"
#include "FWCore/Framework/interface/Event.h"
#include "FWCore/Framework/interface/MakerMacros.h"
#include "FWCore/ParameterSet/interface/ParameterSet.h"

#include "SimDataFormats/Track/interface/SimTrack.h"
#include "SimDataFormats/Vertex/interface/SimVertex.h"
#include "SimDataFormats/Track/interface/SimTrackContainer.h"
#include "SimDataFormats/Vertex/interface/SimVertexContainer.h"

#include "RecoLocalCalo/HGCalRecAlgos/interface/RecHitTools.h"

#include "DataFormats/Common/interface/Ptr.h"
#include "DataFormats/Common/interface/View.h"
#include "SimDataFormats/CaloHit/interface/PCaloHitContainer.h"
#include "SimDataFormats/CaloHit/interface/PCaloHit.h"
#include "DataFormats/DetId/interface/DetId.h"

#include <iterator> // For std::forward_iterator_tag
#include <cstddef>  // For std::ptrdiff_t


#define EDM_ML_DEBUG

struct Hit {
    Hit(){}
    Hit(float x, float y, float z, float t, float energy, int trackid) :
        x_(x), y_(y), z_(z), t_(t), energy_(energy), trackid_(trackid) {}
    ~Hit() {}
    float x_;
    float y_;
    float z_;
    float t_;
    float energy_;
    int trackid_;
    };

/* Computes the 'average' position of a list of hits */
GlobalPoint hitcentroid(vector<Hit*> hits){
    if (hits.size()==0) cms::Exception("SimMerging") << "Cannot compute hit centroid for 0 hits";
    else if (hits.size()==1) return GlobalPoint(hits[0]->x_, hits[0]->y_, hits[0]->z_);
    float summedEnergy = 0.;
    for(auto hit : hits) summedEnergy += hit->energy_;
    float center_x = 0.f, center_y = 0.f, center_z = 0.f;
    for(auto hit : hits){
        float weight = hit->energy_/summedEnergy;
        center_x += weight * hit->x_;
        center_y += weight * hit->y_;
        center_z += weight * hit->z_;
        }
    return GlobalPoint(center_x, center_y, center_z);
    }

class Node {
    public:
        Node(){}
        Node(int trackid, float energy, int pdgid) :
            trackid_(trackid), energy_(energy), pdgid_(pdgid), parent_(nullptr),
            hitcentroidCalculated_(false), hitcentroid_(GlobalPoint(0.f,0.f,0.f))
            {}
        ~Node() {}

        /* Standard depth-first-search tree traversal as an iterator */
        struct Iterator {
            using iterator_category = std::forward_iterator_tag;
            using difference_type   = std::ptrdiff_t;
            using value_type        = Node;
            using pointer           = Node*;  // or also value_type*
            using reference         = Node&;  // or also value_type&

            Iterator(pointer ptr, bool verbose=false) :
                m_ptr(ptr), root(ptr), depth_(0), verbose_(verbose) {}

            reference operator*() const { return *m_ptr; }
            pointer operator->() { return m_ptr; }

            Iterator& operator++() {
                if (m_ptr->hasChildren()){
                    if (verbose_) edm::LogVerbatim("SimMerging")
                        << "Track " << m_ptr->trackid_
                        << ": Going to first child " << m_ptr->children_[0]->trackid_
                        ;
                    continuation_.push(m_ptr);
                    m_ptr = m_ptr->children_[0];
                    depth_++;
                    }
                else {
                    if (verbose_) edm::LogVerbatim("SimMerging")
                        << "Track " << m_ptr->trackid_
                        << ": No children, going to next sibling"
                        ;
                    while(true){
                        if (m_ptr == root){
                            if (verbose_) edm::LogVerbatim("SimMerging") << "Back at the root of the iterator; quiting";
                            m_ptr = nullptr;
                            break;
                            }
                        else if (m_ptr->hasNextSibling()){
                            m_ptr = m_ptr->nextSibling();
                            if (verbose_) edm::LogVerbatim("SimMerging") << "Has sibling; going to " << m_ptr->trackid_;
                            break;
                            }
                        if (verbose_) edm::LogVerbatim("SimMerging") << "Has no sibling; proceed popping stack";
                        m_ptr = continuation_.top();
                        continuation_.pop();
                        depth_--;
                        if (verbose_) edm::LogVerbatim("SimMerging") << "Popped back to track " << m_ptr->trackid_;
                        }
                    }
                return *this;
                }
            // Postfix increment
            Iterator operator++(int) { Iterator tmp = *this; ++(*this); return tmp; }
            const int depth(){return depth_;}
            friend bool operator== (const Iterator& a, const Iterator& b) { return a.m_ptr == b.m_ptr; };
            friend bool operator!= (const Iterator& a, const Iterator& b) { return a.m_ptr != b.m_ptr; };  
            private:
                pointer m_ptr;
                pointer root;
                int depth_;
                bool verbose_;
                std::stack<pointer> continuation_;
            };
        Iterator begin(bool verbose=false) { return Iterator(this, verbose); }
        Iterator end() { return Iterator(nullptr); }

        /* Traverses upwards */
        struct IteratorUp {
            using iterator_category = std::forward_iterator_tag;
            using difference_type   = std::ptrdiff_t;
            using value_type        = Node;
            using pointer           = Node*;  // or also value_type*
            using reference         = Node&;  // or also value_type&
            IteratorUp(pointer ptr) : m_ptr(ptr) {}
            reference operator*() const { return *m_ptr; }
            pointer operator->() { return m_ptr; }
            IteratorUp& operator++() {
                m_ptr = (m_ptr->hasParent()) ? m_ptr->parent_ : nullptr ;
                return *this;
                }
            IteratorUp operator++(int) { IteratorUp tmp = *this; ++(*this); return tmp; }
            friend bool operator== (const IteratorUp& a, const IteratorUp& b) { return a.m_ptr == b.m_ptr; };
            friend bool operator!= (const IteratorUp& a, const IteratorUp& b) { return a.m_ptr != b.m_ptr; };  
            private: pointer m_ptr;
            };
        IteratorUp begin_up() { return IteratorUp(this); }
        IteratorUp end_up() { return IteratorUp(nullptr); }

        void setParent(Node* parent) {
            // edm::LogVerbatim("SimMerging")
            //     << "Track " << trackid_
            //     << ": setting parent " << parent->trackid_
            //     ;
            parent_ = parent;
            }

        void addChild(Node* child) {
            // edm::LogVerbatim("SimMerging")
            //     << "Track " << trackid_
            //     << ": adding child " << child->trackid_
            //     << " (children_.size()=" << children_.size()
            //     << ")"
            //     ;
            children_.push_back(child);
            }

        void addHit(Hit* hit) {
            // edm::LogVerbatim("SimMerging")
            //     << "Track " << trackid_
            //     << ": adding child " << child->trackid_
            //     << " (children_.size()=" << children_.size()
            //     << ")"
            //     ;
            hits_.push_back(hit);
            }

        int nhits(){ return hits_.size(); }
        bool hasHits(){ return nhits() > 0; }
        bool isLeaf(){ return children_.empty(); }
        bool hasChildren(){ return !(children_.empty()); }
        bool hasParent(){ return parent_ != nullptr; }

        bool hasNextSibling(){
            if (!parent_){
                // There is no parent
                return false;
                }
            else if (parent_->children_.back() == this){
                // This was the last child
                return false;
                }
            // else if ( (int)(parent_->children_.size()-1) == iChild_){
            //     // This was the last child
            //     return false;
            //     }
            return true;
            }

        Node* nextSibling(){
            if (hasNextSibling()){
                vector<Node*>::iterator sibling = std::find(
                    parent_->children_.begin(), parent_->children_.end(), this
                    );
                sibling++; // advance once
                if (sibling == parent_->children_.end())
                    // This shouldn't happen
                    return nullptr;
                return *sibling;
                }
            return nullptr;
            }

        /* Traverses tree and builds string representation */
        std::string stringrep(){
            std::stringstream ss;
            int nTracks = 0;
            int nHits = 0;
            for (Node::Iterator it = begin(); it != end(); it++){
                Node& node = (*it);
                for (int i = 0; i < it.depth(); ++i){
                    ss << "--";
                    }
                ss
                    << "Track " << node.trackid_
                    << " (" << node.nhits() << " hits)"
                    << "\n";
                nTracks++;
                nHits += node.nhits();
                }
            ss << "In total " << nTracks << " tracks with " << nHits << " hits";
            return ss.str();
            }

        /* A node is a 'leaf parent' if it has children, and all those children are leafs  */
        bool isLeafParent(){
            // A leaf itself is not a leaf parent
            if (isLeaf()) return false;
            for(auto child : children_){
                if (child->hasChildren()) return false;
                }
            return true;
            }

        /* Uses a boolean as a guard against unnecessarily recomputing the hit centroid */
        GlobalPoint hitcentroid(){
            if (hitcentroidCalculated_) return hitcentroid_;
            return recomputeHitcentroid();
            }

        /* Force recomputes the hit centroid */
        GlobalPoint recomputeHitcentroid(){
            hitcentroid_ = ::hitcentroid(hits_);
            hitcentroidCalculated_ = true;
            return hitcentroid_;
            }

        int trackid_;
        float energy_;
        int pdgid_;
        Node * parent_;
        vector<Node*> children_;
        vector<Hit*> hits_;
        bool hitcentroidCalculated_;
        GlobalPoint hitcentroid_;
    };

/* Remove a node from its parent's children vector */
void break_from_parent(Node* node){
    if (!(node->hasParent())) cms::Exception("SimMerging") << "Cannot remove root node";
    vector<Node*>& parents_children = node->parent_->children_;
    // erase-remove idiom: https://en.wikipedia.org/wiki/Erase%E2%80%93remove_idiom#Example
    parents_children.erase(
        std::remove(parents_children.begin(), parents_children.end(), node),
        parents_children.end()
        );
    }

/* Breaks node from parent but moves its children to the children of the parent */
void remove_intermediate_node(Node* node){
    Node* parent = node->parent_;
    break_from_parent(node);
    // Move children of the now-removed node to its parent
    for (auto child : node->children_){
        parent->addChild(child);
        child->setParent(parent);
        }
    }

// _______________________________________________
// Some functions for traversal by recursion
// These first build the whole traversal in a vector
// (as pointers, so memory usage is not too bad)

/* Does depth first search traversal by using recursion, but not as an iterator */
void _dfs_recursion(
        Node* node,
        vector<pair<Node*, int>>& returnable,
        int depth
        )
    {
    returnable.push_back(std::make_pair(node, depth));
    for (auto child : node->children_){
        _dfs_recursion(child, returnable, depth+1);
        }
    }

/*
Wrapper around the recursive version that only takes a node as input.
Returns a vector of pair<node, depth>.
Useful if you want to keep the whole traversal in memory; usually you
will want to use the iterator version of the Node class.
*/
vector<pair<Node*, int>> dfs(Node* root){
    vector<pair<Node*, int>> returnable;
    _dfs_recursion(root, returnable, 0);
    return returnable;
    }

/* String representation of dfs traversal (keeps whole traversal in memory) */
std::string dfs_stringrep(Node* root){
    std::stringstream ss;
    for (auto node_depth_pair : dfs(root)){
        for (int i = 0; i < node_depth_pair.second; ++i){
            ss << "--";
            }
        ss
            << "Track " << node_depth_pair.first->trackid_
            << " (" << node_depth_pair.first->nhits() << " hits)"
            << "\n";
        }
    return ss.str();
    }

/* Remove single-child no-hit tracks (i.e. intermediate tracks) */
void trim_tree(Node* root){
    // Traverse once and note all tracks that should be kept:
    // Either a track that has hits, or an ancestor thereof
    std::set<int> trackids_with_hits_or_parents_thereof;
    for (auto& node : *root){
        if (!(node.hasHits())) continue;
        // Iterate upwards and save in the set
        for (auto it=node.begin_up(); it!=node.end_up(); it++){
            trackids_with_hits_or_parents_thereof.insert(it->trackid_);
            }
        }
    // Now remove all nodes not in the set
    // We'll be modifying parent-child relationships mid-loop, so we have to be a little
    // careful
    auto it=root->begin(true);
    while(it!=root->end()){
        Node& node = (*it);
        if (!(trackids_with_hits_or_parents_thereof.count(node.trackid_))){
            // First remove children so the iterator will go to the next sibling
            node.children_.clear();
            // Advance to next sibling (or further up the chain)
            it++;
            // Then break from parent (if doing this before advancing the order gets messed up)
            break_from_parent(&node);
            }
        else{
            it++;
            }
        }
    // // Debug printout
    // edm::LogVerbatim("SimMerging") << "Printing root " << root->trackid_ << " after step1 trimming";
    // edm::LogVerbatim("SimMerging") << root->stringrep();
    // Second trimming step: Remove 'intermediate' tracks
    // (i.e. tracks with no hits, 1 child, and 1 parent)
    // In this case it's easier to put the whole traversal in memory first,
    // and avoid modifying relationships mid-loop
    for (auto node_depth_pair : dfs(root)){
        Node* node = node_depth_pair.first;
        if (node->hasParent() && (node->children_.size()==1) && !(node->hasHits())){
            remove_intermediate_node(node);
            }
        }
    }


/* Compute a distance measure between two nodes: now simply distance between the hit centroids */
float distance(Node* left, Node* right){
    GlobalPoint p1 = left->hitcentroid(), p2 = right->hitcentroid();
    return std::sqrt(
        std::pow(p1.x()-p2.x(),2) + std::pow(p1.y()-p2.y(),2) + std::pow(p1.z()-p2.z(),2)
        );
    }

bool merge_leafparent_Mar03(Node* leafparent, float maxr=10.){
    edm::LogVerbatim("SimMerging") << "  Merging leafparent " << leafparent->trackid_;
    bool didUpdate = false;
    // Copy list of potentially mergeable nodes
    vector<Node*> mergeable = leafparent->children_;
    leafparent->children_.clear();
    // Parent itself can be mergeable, if it has hits and is not a root
    if (leafparent->hasParent() && leafparent->hasHits()) mergeable.push_back(leafparent);
    // Start merging
    while(true){
        bool didUpdateThisIteration = false;
        float minr = maxr;
        pair<Node*,Node*> pairToMerge;
        // Compute all distances between clusters
        int nMergeable = mergeable.size();
        for (int i = 0; i < nMergeable; ++i){
            Node* left = mergeable[i];
            for (int j = i+1; j < nMergeable; ++j){
                Node* right = mergeable[j];
                float r = distance(left, right);
                if (r < minr){
                    minr = r;
                    pairToMerge = (left->energy_ > right->energy_) ?
                        std::make_pair(left, right) : std::make_pair(right, left);
                    didUpdate = true;
                    didUpdateThisIteration = true;
                    }
                }
            }
        if (!didUpdateThisIteration) break; // Nothing to merge this iteration
        // Now do the merging
        edm::LogVerbatim("SimMerging")
            << "    Merging " << pairToMerge.second->trackid_
            << " into " << pairToMerge.first->trackid_
            ;
        // Move children
        for(auto child : pairToMerge.second->children_){
            pairToMerge.first->addChild(child);
            child->setParent(pairToMerge.first);
            }
        pairToMerge.second->children_.clear();
        // Move hits
        for(auto hit : pairToMerge.second->hits_) pairToMerge.first->addHit(hit);
        pairToMerge.second->hits_.clear();
        // Delete the merged-away node
        break_from_parent(pairToMerge.second);
        mergeable.erase(
            std::remove(mergeable.begin(), mergeable.end(), pairToMerge.second),
            mergeable.end()
            );
        // Recompute the hitcentroid for newly merged node, now that it has more hits
        pairToMerge.first->recomputeHitcentroid();
        }
    // Make a string representation of the mergeable nodes for debugging
    std::string mergeableStr = "";
    if(mergeable.size()){
        std::stringstream ss;
        for(auto child : mergeable) ss << child->trackid_ << ", ";
        mergeableStr = ss.str();
        mergeableStr.pop_back(); mergeableStr.pop_back(); // Remove trailing comma
        }
    // All possible merging now done;
    // Next steps depend on whether the passed node was a root
    if (!(leafparent->hasParent())){
        leafparent->children_ = mergeable;
        if(didUpdate) {
            // Simply overwrite with the merged nodes
            edm::LogVerbatim("SimMerging")
                << "    Root " << leafparent->trackid_
                << " is set to have the following children: "
                << mergeableStr;
            }
        else{
            edm::LogVerbatim("SimMerging")
                << "    Root " << leafparent->trackid_
                << ": no further merging possible";
            }
        return didUpdate;
        }
    else {
        // Special case: If the leafparent had no hits (and was thus not included as
        // a mergeable node), AND all nodes were merged into one cluster, assign the 
        // pdgid of the leafparent to the remaining node
        if(
            !(leafparent->hasHits())
            && mergeable.size()==1
            && mergeable[0]->pdgid_!=leafparent->pdgid_
            ){
            edm::LogVerbatim("SimMerging")
                << "    Using leafparent pdgid " << leafparent->pdgid_
                << " for track " << mergeable[0]->trackid_
                << " (rather than " << mergeable[0]->pdgid_
                << ") since all nodes were merged into one";
            mergeable[0]->pdgid_ = leafparent->pdgid_;
            }
        // Replace the node in the parent's children list with all merged nodes
        Node* parent = leafparent->parent_;
        edm::LogVerbatim("SimMerging")
            << "    Adding the following children to parent " << parent->trackid_
            << ": " << mergeableStr;
        break_from_parent(leafparent);
        for(auto child : mergeable){
            parent->addChild(child);
            child->setParent(parent);
            }
        return true;
        }
    }

void merging_algo_Mar03(Node* root){
    int iIteration = -1;
    bool didUpdate = true;
    while(didUpdate){
        iIteration++;
        edm::LogVerbatim("SimMerging") << "Iteration " << iIteration;
        // Build list of leaf parents in memory
        vector<Node*> leafparents;
        for (auto& node : *root){
            if (!(node.isLeafParent())) continue;
            leafparents.push_back(&node);
            }
        for (auto node : leafparents){
            didUpdate = merge_leafparent_Mar03(node);
            }
        }
    edm::LogVerbatim("SimMerging") << "Done after iteration " << iIteration;
    }

// _______________________________________________


class simmerger : public edm::stream::EDProducer<> {
    public:
        explicit simmerger(const edm::ParameterSet&);
        ~simmerger() {}
    private:
        virtual void produce(edm::Event&, const edm::EventSetup&) override;
        void beginRun(const edm::Run&, const edm::EventSetup&) override {}
        hgcal::RecHitTools hgcalRecHitToolInstance_ ;
        edm::EDGetTokenT<edm::View<PCaloHit>> hgcalEEHitsToken_;
        edm::EDGetTokenT<edm::View<PCaloHit>> hgcalHEfrontHitsToken_;
        edm::EDGetTokenT<edm::View<PCaloHit>> hgcalHEbackHitsToken_;
        edm::EDGetTokenT<edm::SimTrackContainer> tokenSimTracks;
        edm::EDGetTokenT<edm::SimVertexContainer> tokenSimVertices;
    };


simmerger::simmerger(const edm::ParameterSet& iConfig) :
    hgcalEEHitsToken_(consumes<edm::View<PCaloHit>>(edm::InputTag("g4SimHits", "HGCHitsEE"))),
    hgcalHEfrontHitsToken_(consumes<edm::View<PCaloHit>>(edm::InputTag("g4SimHits", "HGCHitsHEfront"))),
    hgcalHEbackHitsToken_(consumes<edm::View<PCaloHit>>(edm::InputTag("g4SimHits", "HGCHitsHEback"))),
    tokenSimTracks(consumes<edm::SimTrackContainer>(edm::InputTag("g4SimHits"))),
    tokenSimVertices(consumes<edm::SimVertexContainer>(edm::InputTag("g4SimHits")))
    {
    produces<vector<int>>();
    }


void simmerger::produce(edm::Event& iEvent, const edm::EventSetup& iSetup) {  
    edm::ESHandle<CaloGeometry> geom;
    iSetup.get<CaloGeometryRecord>().get(geom);
    hgcalRecHitToolInstance_.setGeometry(*geom);

    std::unique_ptr<vector<int>> output(new vector<int>);

    // Create Hit instances
    vector<Hit> hits;
    vector<edm::EDGetTokenT<edm::View<PCaloHit>>> tokens = {
        hgcalEEHitsToken_,
        hgcalHEfrontHitsToken_,
        hgcalHEbackHitsToken_
        };
    std::set<int> trackids_with_hits;
    for (edm::EDGetTokenT<edm::View<PCaloHit>> token : tokens ) {
        edm::Handle< edm::View<PCaloHit> > handle;
        iEvent.getByToken(token, handle);
        for (auto const & hit : handle->ptrs() ) {
            DetId id = hit->id();
            GlobalPoint position = hgcalRecHitToolInstance_.getPosition(id);
            hits.push_back(Hit(
                position.x(), position.y(), position.z(),
                hit->time(), hit->energy(), hit->geantTrackId()
                ));
            trackids_with_hits.insert(hit->geantTrackId());
            }
        }

    // Build the tree
    edm::Handle<edm::SimTrackContainer> handleSimTracks;
    iEvent.getByLabel("g4SimHits", handleSimTracks);
    edm::Handle<edm::SimVertexContainer> handleSimVertices;
    iEvent.getByLabel("g4SimHits", handleSimVertices);

    edm::LogVerbatim("SimMerging") << "Building map";
    map<int, Node> trackid_to_node;
    for(const auto& track : *(handleSimTracks.product())){
        trackid_to_node.emplace(track.trackId(), Node(track.trackId(), track.momentum().E(), track.type()));
        edm::LogVerbatim("SimMerging")
            << "track.trackId()=" << track.trackId()
            << ", node->trackid_=" << trackid_to_node[track.trackId()].trackid_
            ;
        }

    edm::LogVerbatim("SimMerging") << "Adding hits to nodes";
    for (auto& hit : hits){
        trackid_to_node[hit.trackid_].addHit(&hit);
        }

    edm::LogVerbatim("SimMerging") << "Building tree";
    vector<Node*> roots;
    for(const auto& track : *(handleSimTracks.product())){
        int trackid = track.trackId();
        Node* node = &(trackid_to_node[trackid]);

        edm::LogVerbatim("SimMerging")
            << "track.trackId()=" << trackid
            << ", node->trackid_=" << node->trackid_;

        // Have to get parent info via the SimVertex
        SimVertex vertex = handleSimVertices.product()->at(track.vertIndex());
        bool hasParent = !(vertex.noParent());
        int parentid = vertex.parentIndex();
        // Build the tree
        edm::LogVerbatim("SimMerging") << "hasParent=" << hasParent;
        if (hasParent){
            edm::LogVerbatim("SimMerging")
                << "Setting parent->child relationship: "
                << parentid << " -> " << trackid
                ;
            auto it = trackid_to_node.find(parentid);
            if (it != trackid_to_node.end()){
                // edm::LogVerbatim("SimMerging") << "Found it!";
                Node* parent = &(it->second);
                node->setParent(parent);
                parent->addChild(node);
                }
            else{
                throw cms::Exception("Unknown")
                    << "Track id " << parentid
                    << " is not in the map"
                    ;
                }
            }
        else{
            edm::LogVerbatim("SimMerging") << "Found root: " << node->trackid_;
            roots.emplace_back(node);
            }
        }

    Node* root = roots[0];
    edm::LogVerbatim("SimMerging") << "Printing root " << root->trackid_;
    edm::LogVerbatim("SimMerging") << root->stringrep() << "\n";

    // edm::LogVerbatim("SimMerging") << "Printing root " << root->trackid_ << " by recursive traversal";
    // edm::LogVerbatim("SimMerging") << dfs_stringrep(root);

    edm::LogVerbatim("SimMerging") << "Trimming tree...";
    trim_tree(root);
    edm::LogVerbatim("SimMerging") << "Printing root " << root->trackid_ << " after trimming";
    edm::LogVerbatim("SimMerging") << root->stringrep() << "\n";

    edm::LogVerbatim("SimMerging") << "Running merging algo...";
    merging_algo_Mar03(root);
    edm::LogVerbatim("SimMerging") << "Printing root " << root->trackid_ << " after merging_algo_Mar03";
    edm::LogVerbatim("SimMerging") << root->stringrep() << "\n";
    
    iEvent.put(std::move(output));
    }

DEFINE_FWK_MODULE(simmerger);