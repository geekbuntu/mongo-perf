#include <mongo/client/dbclient.h>
#include <iostream>
#include <cstdlib>
#include <vector>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/bind.hpp>
#include <boost/foreach.hpp>

#ifndef _WIN32
#include <cxxabi.h>
#endif

using namespace std;
using namespace mongo;


namespace {
    const int thread_nums[] = {1,2,4,5,8,10};
    const int max_threads = 10;
    // Global connections
    DBClientConnection conn[max_threads];

    const char* db = "benchmarks";
    const char* ns = "benchmarks.collection";
    const char* coll = "collection";

    // passed in as argument
    int iterations;

    struct TestBase{
        virtual void run(int thread, int nthreads) = 0;
        virtual void reset() = 0;
        virtual string name() = 0;
        virtual ~TestBase() {}
    };

    template <typename T>
    struct Test: TestBase{
        virtual void run(int thread, int nthreads){
            test.run(thread, nthreads);
            conn[thread].getLastError(); //wait for operation to complete
        }
        virtual void reset(){
            test.reset();
        }
        
        virtual string name(){
            //from mongo::regression::demangleName()
#ifdef _WIN32
            return typeid(T).name();
#else
            int status;

            char * niceName = abi::__cxa_demangle(typeid(T).name(), 0, 0, &status);
            if ( ! niceName )
                return typeid(T).name();

            string s = niceName;
            free(niceName);
            return s;
#endif
        }

        T test;
    };

    struct TestSuite{
            template <typename T>
            void add(){
                tests.push_back(new Test<T>());
            }
            void run(){
                for (vector<TestBase*>::iterator it=tests.begin(), end=tests.end(); it != end; ++it){
                    TestBase* test = *it;
                    boost::posix_time::ptime start, end; //reused

                    cerr << "########## " << test->name() << " ##########" << endl;

                    BSONObjBuilder results;

                    double one_micros;
                    BOOST_FOREACH(int nthreads, thread_nums){
                        test->reset();
                        start = boost::posix_time::microsec_clock::universal_time();
                        launch_subthreads(nthreads, test);
                        end = boost::posix_time::microsec_clock::universal_time();
                        double micros = (end-start).total_microseconds() / 1000000.0;

                        if (nthreads == 1) 
                            one_micros = micros;

                        results.append(BSONObjBuilder::numStr(nthreads),
                                       BSON( "time" << micros
                                          << "ops_per_sec" << iterations / micros
                                          << "speedup" << one_micros / micros
                                          ));
                    }

                    BSONObj out =
                        BSON( "name" << test->name()
                           << "results" << results.obj()
                           );
                    cout << out.jsonString(Strict) << endl;
                }
            }
        private:
            vector<TestBase*> tests;

            void launch_subthreads(int remaining, TestBase* test, int total=-1){ //total = remaining
                if (!remaining) return;

                if (total == -1)
                    total = remaining;

                boost::thread athread(boost::bind(&TestBase::run, test, total-remaining, total));

                launch_subthreads(remaining - 1, test, total);

                athread.join();
            }
    };

    void clearDB(){
        conn[0].dropDatabase(db);
        conn[0].getLastError();
    }
}

namespace Overhead{
    // this tests the overhead of the system
    struct DoNothing{
        void run(int t, int n) {}
        void reset(){ clearDB(); }
    };
}

namespace Insert{
    struct Base{
        void reset(){ clearDB(); }
    };

    struct Empty : Base{
        void run(int t, int n) {
            for (int i=0; i < iterations / n; i++){
                conn[t].insert(ns, BSONObj());
            }
        }
    };

    template <int BatchSize>
    struct EmptyBatched : Base{
        void run(int t, int n) {
            for (int i=0; i < iterations / BatchSize / n; i++){
                vector<BSONObj> objs(BatchSize);
                conn[t].insert(ns, objs);
            }
        }
    };

    struct EmptyCapped : Base{
        void run(int t, int n) {
            for (int i=0; i < iterations / n; i++){
                conn[t].insert(ns, BSONObj());
            }
        }
        void reset(){
            clearDB();
            conn[0].createCollection(ns, 32 * 1024, true);
        }
    };

    struct JustID : Base{
        void run(int t, int n) {
            for (int i=0; i < iterations / n; i++){
                BSONObjBuilder b;
                b << GENOID;
                conn[t].insert(ns, b.obj());
            }
        }
    };

    struct IntID : Base{
        void run(int t, int n) {
            int base = t * (iterations/n);
            for (int i=0; i < iterations / n; i++){
                conn[t].insert(ns, BSON("_id" << base + i));
            }
        }
    };

    struct IntIDUpsert : Base{
        void run(int t, int n) {
            int base = t * (iterations/n);
            for (int i=0; i < iterations / n; i++){
                conn[t].update(ns, BSON("_id" << base + i), BSONObj(), true);
            }
        }
    };

    struct JustNum : Base{
        void run(int t, int n) {
            int base = t * (iterations/n);
            for (int i=0; i < iterations / n; i++){
                conn[t].insert(ns, BSON("x" << base + i));
            }
        }
    };

    struct JustNumIndexedBefore : Base{
        void run(int t, int n) {
            conn[t].ensureIndex(ns, BSON("x" << 1));
            int base = t * (iterations/n);
            for (int i=0; i < iterations / n; i++){
                conn[t].insert(ns, BSON("x" << base + i));
            }
        }
    };

    struct JustNumIndexedAfter : Base{
        void run(int t, int n) {
            int base = t * (iterations/n);
            for (int i=0; i < iterations / n; i++){
                conn[t].insert(ns, BSON("x" << base + i));
            }
            conn[t].ensureIndex(ns, BSON("x" << 1));
        }
    };

    struct NumAndID : Base{
        void run(int t, int n) {
            int base = t * (iterations/n);
            for (int i=0; i < iterations / n; i++){
                BSONObjBuilder b;
                b << GENOID;
                b << "x" << base+i;
                conn[t].insert(ns, b.obj());
            }
        }
    };
}

namespace Update{
    struct Base{
        void reset(){ clearDB(); }
    };

    struct IncNoIndexUpsert : Base{
        void run(int t, int n) {
            const int incs = iterations/n/100;
            for (int i=0; i<100; i++){
                for (int j=0; j<incs; j++){
                    conn[t].update(ns, BSON("_id" << i), BSON("$inc" << BSON("count" << 1)), 1);
                }
            }
        }
    };
    struct IncWithIndexUpsert : Base{
        void reset(){ clearDB(); conn[0].ensureIndex(ns, BSON("count" << 1));}
        void run(int t, int n) {
            const int incs = iterations/n/100;
            for (int i=0; i<100; i++){
                for (int j=0; j<incs; j++){
                    conn[t].update(ns, BSON("_id" << i), BSON("$inc" << BSON("count" << 1)), 1);
                }
            }
        }
    };
    struct IncNoIndex : Base{
        void reset(){
            clearDB(); 
            for (int i=0; i<100; i++)
                conn[0].insert(ns, BSON("_id" << i << "count" << 0));
        }
        void run(int t, int n) {
            const int incs = iterations/n/100;
            for (int i=0; i<100; i++){
                for (int j=0; j<incs; j++){
                    conn[t].update(ns, BSON("_id" << i), BSON("$inc" << BSON("count" << 1)));
                }
            }
        }
    };
    struct IncWithIndex : Base{
        void reset(){
            clearDB(); 
            conn[0].ensureIndex(ns, BSON("count" << 1));
            for (int i=0; i<100; i++)
                conn[0].insert(ns, BSON("_id" << i << "count" << 0));
        }
        void run(int t, int n) {
            const int incs = iterations/n/100;
            for (int i=0; i<100; i++){
                for (int j=0; j<incs; j++){
                    conn[t].update(ns, BSON("_id" << i), BSON("$inc" << BSON("count" << 1)));
                }
            }
        }
    };
    struct IncNoIndex_QueryOnSecondary : Base{
        void reset(){
            clearDB(); 
            conn[0].ensureIndex(ns, BSON("i" << 1));
            for (int i=0; i<100; i++)
                conn[0].insert(ns, BSON("_id" << i << "i" << i << "count" << 0));
        }
        void run(int t, int n) {
            const int incs = iterations/n/100;
            for (int i=0; i<100; i++){
                for (int j=0; j<incs; j++){
                    conn[t].update(ns, BSON("i" << i), BSON("$inc" << BSON("count" << 1)));
                }
            }
        }
    };
    struct IncWithIndex_QueryOnSecondary : Base{
        void reset(){
            clearDB(); 
            conn[0].ensureIndex(ns, BSON("count" << 1));
            conn[0].ensureIndex(ns, BSON("i" << 1));
            for (int i=0; i<100; i++)
                conn[0].insert(ns, BSON("_id" << i << "i" << i << "count" << 0));
        }
        void run(int t, int n) {
            const int incs = iterations/n/100;
            for (int i=0; i<100; i++){
                for (int j=0; j<incs; j++){
                    conn[t].update(ns, BSON("i" << i), BSON("$inc" << BSON("count" << 1)));
                }
            }
        }
    };
}

namespace Queries{
    struct Empty{
        void reset() {
            clearDB();
            for (int i=0; i < iterations; i++){
                conn[0].insert(ns, BSONObj());
            }
            conn[0].getLastError();
        }

        void run(int t, int n){
            int chunk = iterations / n;
            auto_ptr<DBClientCursor> cursor = conn[t].query(ns, BSONObj(), chunk, chunk*t);
            cursor->itcount();
        }
    };

    struct HundredTableScans{
        void reset() {
            clearDB();
            for (int i=0; i < iterations; i++){
                conn[0].insert(ns, BSONObj());
            }
            conn[0].getLastError();
        }

        void run(int t, int n){
            for (int i=0; i < 100/n; i++){
                conn[t].findOne(ns, BSON("does_not_exist" << i));
            }
        }
    };

    struct IntID{
        void reset() {
            clearDB();
            for (int i=0; i < iterations; i++){
                conn[0].insert(ns, BSON("_id" << i));
            }
            conn[0].getLastError();
        }

        void run(int t, int n){
            int chunk = iterations / n;
            auto_ptr<DBClientCursor> cursor = conn[t].query(ns, BSONObj(), chunk, chunk*t);
            cursor->itcount();
        }
    };

    struct IntIDRange{
        void reset() {
            clearDB();
            for (int i=0; i < iterations; i++){
                conn[0].insert(ns, BSON("_id" << i));
            }
            conn[0].getLastError();
        }

        void run(int t, int n){
            int chunk = iterations / n;
            auto_ptr<DBClientCursor> cursor = conn[t].query(ns, BSON("_id" << GTE << chunk*t << LT << chunk*(t+1)));
            cursor->itcount();
        }
    };

    struct IntIDFindOne{
        void reset() {
            clearDB();
            for (int i=0; i < iterations; i++){
                conn[0].insert(ns, BSON("_id" << i));
            }
            conn[0].getLastError();
        }

        void run(int t, int n){
            int base = t * (iterations/n);
            for (int i=0; i < iterations / n; i++){
                conn[t].findOne(ns, BSON("_id" << base + i));
            }
        }
    };

    struct IntNonID{
        void reset() {
            clearDB();
            conn[0].ensureIndex(ns, BSON("x" << 1));
            for (int i=0; i < iterations; i++){
                conn[0].insert(ns, BSON("x" << i));
            }
            conn[0].getLastError();
        }

        void run(int t, int n){
            int chunk = iterations / n;
            auto_ptr<DBClientCursor> cursor = conn[t].query(ns, BSONObj(), chunk, chunk*t);
            cursor->itcount();
        }
    };

    struct IntNonIDRange{
        void reset() {
            clearDB();
            conn[0].ensureIndex(ns, BSON("x" << 1));
            for (int i=0; i < iterations; i++){
                conn[0].insert(ns, BSON("x" << i));
            }
            conn[0].getLastError();
        }

        void run(int t, int n){
            int chunk = iterations / n;
            auto_ptr<DBClientCursor> cursor = conn[t].query(ns, BSON("x" << GTE << chunk*t << LT << chunk*(t+1)));
            cursor->itcount();
        }
    };

    struct IntNonIDFindOne{
        void reset() {
            clearDB();
            conn[0].ensureIndex(ns, BSON("x" << 1));
            for (int i=0; i < iterations; i++){
                conn[0].insert(ns, BSON("x" << i));
            }
            conn[0].getLastError();
        }

        void run(int t, int n){
            int base = t * (iterations/n);
            for (int i=0; i < iterations / n; i++){
                conn[t].findOne(ns, BSON("x" << base + i));
            }
        }
    };

    struct RegexPrefixFindOne{
        RegexPrefixFindOne(){
            for (int i=0; i<100; i++)
                nums[i] = "^" + BSONObjBuilder::numStr(i+1);
        }
        void reset() {
            clearDB();
            conn[0].ensureIndex(ns, BSON("x" << 1));
            for (int i=0; i < iterations; i++){
                conn[0].insert(ns, BSON("x" << BSONObjBuilder::numStr(i)));
            }
            conn[0].getLastError();
        }

        void run(int t, int n){
            for (int i=0; i < iterations / n / 100; i++){
                for (int j=0; j<100; j++){
                    BSONObjBuilder b;
                    b.appendRegex("x", nums[j]);
                    conn[t].findOne(ns, b.obj());
                }
            }
        }
        string nums[100];
    };

    struct TwoIntsBothGood{
        void reset() {
            clearDB();
            conn[0].ensureIndex(ns, BSON("x" << 1));
            conn[0].ensureIndex(ns, BSON("y" << 1));
            for (int i=0; i < iterations; i++){
                conn[0].insert(ns, BSON("x" << i << "y" << (iterations-i)));
            }
            conn[0].getLastError();
        }

        void run(int t, int n){
            int base = t * (iterations/n);
            for (int i=0; i < iterations / n; i++){
                conn[t].findOne(ns, BSON("x" << base + i << "y" << (iterations-(base+i))));
            }
        }
    };

    struct TwoIntsFirstGood{
        void reset() {
            clearDB();
            conn[0].ensureIndex(ns, BSON("x" << 1));
            conn[0].ensureIndex(ns, BSON("y" << 1));
            for (int i=0; i < iterations; i++){
                conn[0].insert(ns, BSON("x" << i << "y" << (i%13)));
            }
            conn[0].getLastError();
        }

        void run(int t, int n){
            int base = t * (iterations/n);
            for (int i=0; i < iterations / n; i++){
                conn[t].findOne(ns, BSON("x" << base + i << "y" << ((base+i)%13)));
            }
        }
    };

    struct TwoIntsSecondGood{
        void reset() {
            clearDB();
            conn[0].ensureIndex(ns, BSON("x" << 1));
            conn[0].ensureIndex(ns, BSON("y" << 1));
            for (int i=0; i < iterations; i++){
                conn[0].insert(ns, BSON("x" << (i%13) << "y" << i));
            }
            conn[0].getLastError();
        }

        void run(int t, int n){
            int base = t * (iterations/n);
            for (int i=0; i < iterations / n; i++){
                conn[t].findOne(ns, BSON("x" << ((base+i)%13) << "y" << base+i));
            }
        }
    };
    struct TwoIntsBothBad{
        void reset() {
            clearDB();
            conn[0].ensureIndex(ns, BSON("x" << 1));
            conn[0].ensureIndex(ns, BSON("y" << 1));
            for (int i=0; i < iterations; i++){
                conn[0].insert(ns, BSON("x" << (i%503) << "y" << (i%509))); // both are prime
            }
            conn[0].getLastError();
        }

        void run(int t, int n){
            int base = t * (iterations/n);
            for (int i=0; i < iterations / n; i++){
                conn[t].findOne(ns, BSON("x" << ((base+i)%503) << "y" << ((base+i)%509)));
            }
        }
    };

}

namespace{
    struct TheTestSuite : TestSuite{
        TheTestSuite(){
            //add< Overhead::DoNothing >();

            add< Insert::Empty >();
            add< Insert::EmptyBatched<2> >();
            add< Insert::EmptyBatched<10> >();
            //add< Insert::EmptyBatched<100> >();
            //add< Insert::EmptyBatched<1000> >();
            //add< Insert::EmptyCapped >();
            //add< Insert::JustID >();
            add< Insert::IntID >();
            add< Insert::IntIDUpsert >();
            //add< Insert::JustNum >();
            add< Insert::JustNumIndexedBefore >();
            add< Insert::JustNumIndexedAfter >();
            //add< Insert::NumAndID >();
            
            add< Update::IncNoIndexUpsert >();
            add< Update::IncWithIndexUpsert >();
            add< Update::IncNoIndex >();
            add< Update::IncWithIndex >();
            add< Update::IncNoIndex_QueryOnSecondary >();
            add< Update::IncWithIndex_QueryOnSecondary >();

            //add< Queries::Empty >();
            add< Queries::HundredTableScans >();
            //add< Queries::IntID >();
            add< Queries::IntIDRange >();
            add< Queries::IntIDFindOne >();
            //add< Queries::IntNonID >();
            add< Queries::IntNonIDRange >();
            add< Queries::IntNonIDFindOne >();
            //add< Queries::RegexPrefixFindOne >();
            //add< Queries::TwoIntsBothBad >();
            //add< Queries::TwoIntsBothGood >();
            //add< Queries::TwoIntsFirstGood >();
            //add< Queries::TwoIntsSecondGood >();
        }
    } theTestSuite;
}

int main(int argc, const char **argv){
    if (argc != 3){
        cout << argv[0] << ": [port] [iterations]" << endl;
        return 1;
    }

    for (int i=0; i < max_threads; i++){
        string errmsg;
        if ( ! conn[i].connect( string( "127.0.0.1:" ) + argv[1], errmsg ) ) {
            cout << "couldn't connect : " << errmsg << endl;
            return 1;
        }
    }

    iterations = atoi(argv[2]);

    theTestSuite.run();

    return 0;
}
