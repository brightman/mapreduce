// MapReduce library
//
//  Copyright (C) 2009 Craig Henderson.
//  cdm.henderson@gmail.com
//
//  Use, modification and distribution is subject to the
//  Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// For more information, see http://craighenderson.co.uk/mapreduce/
//
 
#ifndef MAPREDUCE_IN_MEMORY_INTERMEDIATES_HPP
#define MAPREDUCE_IN_MEMORY_INTERMEDIATES_HPP

#include <boost/iterator/iterator_facade.hpp>

namespace mapreduce {

namespace intermediates {

template<typename MapTask, typename ReduceTask>
class reduce_null_output
{
  public:
    reduce_null_output()
    { }

    reduce_null_output(std::string const &/*output_filespec*/,
                       unsigned    const  /*partition*/,
                       unsigned    const  /*num_partitions*/)
    {
    }

    void operator()(typename ReduceTask::key_type   const &/*key*/,
                    typename ReduceTask::value_type const &/*value*/)
    {
    }
};


template<
    typename MapTask,
    typename ReduceTask,
    typename PartitionFn=mapreduce::hash_partitioner,
    typename KeyCompare=std::less<typename ReduceTask::key_type> >
class in_memory : boost::noncopyable
{
  private:
    typedef
    std::vector<
        std::map<
            typename ReduceTask::key_type,
            std::list<typename ReduceTask::value_type>,
            KeyCompare > >
    intermediates_t;

  public:
    typedef MapTask    map_task_type;
    typedef ReduceTask reduce_task_type;
    typedef reduce_null_output<MapTask, ReduceTask> store_result_type;
    typedef
    std::pair<
        typename reduce_task_type::key_type,
        typename reduce_task_type::value_type>
    keyvalue_t;

    class const_result_iterator
      : public boost::iterator_facade<
            const_result_iterator,
            keyvalue_t const,
            boost::forward_traversal_tag>
    {
        friend class boost::iterator_core_access;

      protected:
        explicit const_result_iterator(in_memory const *outer)
          : outer_(outer)
        {
            BOOST_ASSERT(outer_);
            iterators_.resize(outer_->num_partitions_);
        }

        const_result_iterator &operator=(const_result_iterator const &other);

        void increment(void)
        {
            ++current_.second;
            if (current_.second == iterators_[current_.first]->second.end())
            {
                if (iterators_[current_.first] != outer_->intermediates_[current_.first].end())
                    ++iterators_[current_.first];

                set_current();
            }
            else
                value_ = std::make_pair(iterators_[current_.first]->first, *current_.second);
        }

        bool const equal(const_result_iterator const &other) const
        {
            if (current_.first == std::numeric_limits<unsigned>::max()  ||  other.current_.first == std::numeric_limits<unsigned>::max())
                return other.current_.first == current_.first;
            return value_ == other.value_;
        }

        const_result_iterator &begin(void)
        {
            for (unsigned loop=0; loop<outer_->num_partitions_; ++loop)
                iterators_[loop] = outer_->intermediates_[loop].begin();
            set_current();
            return *this;
        }

        const_result_iterator &end(void)
        {
            current_.first = std::numeric_limits<unsigned>::max();
            value_ = keyvalue_t();
            iterators_.clear();
            return *this;
        }

        keyvalue_t const &dereference(void) const
        {
            return value_;
        }

        void set_current(void)
        {
            for (current_.first=0; current_.first<outer_->num_partitions_  &&  iterators_[current_.first] == outer_->intermediates_[current_.first].end(); ++current_.first)
                ;
            
            for (unsigned loop=current_.first+1; loop<outer_->num_partitions_; ++loop)
            {
                if (iterators_[loop] != outer_->intermediates_[loop].end()  &&  *iterators_[current_.first] > *iterators_[loop])
                    current_.first = loop;
            }

            if (current_.first == outer_->num_partitions_)
                end();
            else
            {
                current_.second = iterators_[current_.first]->second.begin();
                value_ = std::make_pair(iterators_[current_.first]->first, *current_.second);
            }
        }

      private:
        typedef
        std::vector<typename intermediates_t::value_type::const_iterator>
        iterators_t;

        typedef
        std::pair<
            typename iterators_t::const_iterator,
            typename intermediates_t::value_type::mapped_type::const_iterator>
        current_t;

        in_memory const *outer_;        // parent container
        iterators_t      iterators_;    // iterator group
        keyvalue_t       value_;        // value of current element

        // the current element consists of an index to the partition
        // list, and an iterator within that list
        std::pair<
            unsigned,                   // index of current element
            typename                    // iterator of the sub-element
                intermediates_t::value_type::mapped_type::const_iterator
        > current_;

        friend class in_memory;
    };
    friend class const_result_iterator;

    in_memory(unsigned const num_partitions=1)
      : num_partitions_(num_partitions)
    {
        intermediates_.resize(num_partitions_);
    }

    const_result_iterator begin_results(void) const
    {
        return const_result_iterator(this).begin();
    }

    const_result_iterator end_results(void) const
    {
        return const_result_iterator(this).end();
    }

    void swap(in_memory &other)
    {
        std::swap(intermediates_, other.intermediates_);
    }

    template<typename Callback>
    void reduce(unsigned const partition, Callback &callback)
    {
        typename intermediates_t::value_type map;
        std::swap(map, intermediates_[partition]);

        for (typename intermediates_t::value_type::const_iterator it1=map.begin(); it1!=map.end(); ++it1)
        {
            callback(it1->first, it1->second.begin(), it1->second.end());
        }
    }

    void merge_from(unsigned partition, in_memory &other)
    {
        typedef typename intermediates_t::value_type map_type;

        map_type &map       = intermediates_[partition];
        map_type &other_map = other.intermediates_[partition];

        if (map.size() == 0)
        {
            std::swap(map, other_map);
            return;
        }

        for (typename map_type::iterator it=other_map.begin(); it!=other_map.end(); ++it)
        {
            typename map_type::iterator iti = map.insert(make_pair(it->first, typename map_type::mapped_type())).first;
            std::copy(it->second.begin(), it->second.end(), std::back_inserter(iti->second));
        }
    }

    void merge_from(in_memory &other)
    {
        for (unsigned partition=0; partition<num_partitions_; ++partition)
            merge_from(partition, other);
        other.intermediates_.clear();
    }

    template<typename StoreResult>
    bool const insert(typename reduce_task_type::key_type   const &key,
                      typename reduce_task_type::value_type const &value,
                      StoreResult &store_result)
    {
        store_result(key, value);
        return insert(key, value);
    }

    bool const insert(typename reduce_task_type::key_type   const &key,
                      typename reduce_task_type::value_type const &value)
    {
        size_t const partition = (num_partitions_ == 1)? 0 : partitioner_(key, num_partitions_);
        typename intermediates_t::value_type &map = intermediates_[partition];

        map.insert(
            make_pair(
                key,
                typename intermediates_t::value_type::mapped_type())).first->second.push_back(value);

        return true;
    }

    template<typename FnObj>
    void combine(FnObj &fn_obj)
    {
        intermediates_t intermediates;
        intermediates.resize(num_partitions_);
        std::swap(intermediates_, intermediates);

        for (typename intermediates_t::const_iterator it=intermediates.begin(); it!=intermediates.end(); ++it)
        {
            for (typename intermediates_t::value_type::const_iterator it1=it->begin(); it1!=it->end(); ++it1)
            {
                fn_obj.start(it1->first);
                std::for_each<
                    typename intermediates_t::value_type::mapped_type::const_iterator, FnObj &>(
                        it1->second.begin(),
                        it1->second.end(),
                        fn_obj);
                fn_obj.finish(it1->first, *this);
            }
        }
    }

  private:
    unsigned const  num_partitions_;
    intermediates_t intermediates_;
    PartitionFn     partitioner_;
};


}   // namespace intermediates

}   // namespace mapreduce

#endif  // MAPREDUCE_IN_MEMORY_INTERMEDIATES_HPP
