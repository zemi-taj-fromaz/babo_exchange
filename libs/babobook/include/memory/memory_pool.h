//
// Created by hrcol on 1.7.2026..
//

#ifndef BABOMATCHINGENGINE_MEMORY_POOL_H
#define BABOMATCHINGENGINE_MEMORY_POOL_H

#include <cstddef>
#include <memory>
#include <vector>
#include <utility>
#include <unordered_set>

namespace babo::memory
{


template <typename T>
union PGMemChunk
{
    alignas(T) std::byte element[sizeof(T)];
    PGMemChunk *next;
};


template <typename T, size_t N = 1024>
class AllocatorPool
{
public:

    ~AllocatorPool()
    {
        for (PGMemChunk<T>* chunk : chunkList)
            delete[] chunk;  // Use delete[] to match new[]
    }

    void reserve(size_t reserveSize)
    {
        if (reserveSize < size) return;

        while (reserveSize >= size)
        {
            const size_t blockSize = N >= 2 ? N : size == 0 ? 64 : size;

            auto newBlock = new PGMemChunk<T>[blockSize];
            chunkList.push_back(newBlock);
            size += blockSize;
        }
    }


    template <typename... Args>
    T* allocate(Args&&... args)
    {

        if (freeList)
        {
            auto chunk = freeList;
            freeList = chunk->next;

            T* element = std::construct_at(
                reinterpret_cast<T*>(chunk->element),
                std::forward<Args>(args)...);

            nbElements++;

            return element;
        }

        const size_t index = nbElements++;

        if (index >= size) reserve(index);

        // Track high-water mark for destructor cleanup
        if (index > maxAllocatedIndex)
            maxAllocatedIndex = index;

        // Todo Check if the chunk was created before creating a new element
        PGMemChunk<T>* chunk = getChunk(index);

        return std::construct_at(reinterpret_cast<T*>(chunk->element),
                                 std::forward<Args>(args)...);
    }

    template <typename... Args>
    std::pair<T*, size_t> allocateWithIndex(Args&&... args)
    {
        if (freeList)
        {
            auto chunk = freeList;
            freeList = chunk->next;

            T* ptr = std::construct_at(
                reinterpret_cast<T*>(chunk->element),
                std::forward<Args>(args)...);

            nbElements++;

            // Find the index by calculating from chunk pointer
            size_t index = 0;
            for (size_t i = 0; i < size; i++)
            {
                if (getElement(i) == ptr)
                {
                    index = i;
                    break;
                }
            }

            return {ptr, index};
        }

        const size_t index = nbElements++;

        if (index >= size) reserve(index);

        // Track high-water mark for destructor cleanup
        if (index > maxAllocatedIndex)
            maxAllocatedIndex = index;

        PGMemChunk<T>* chunk = getChunk(index);
        T* ptr = std::construct_at(reinterpret_cast<T*>(chunk->element),
                                   std::forward<Args>(args)...);

        return {ptr, index};
    }

    // Todo add a bulk allocation and deallocation function

    void release(T* pointer)
    {
        if (pointer != nullptr)
        {
            std::destroy_at(pointer);

            reinterpret_cast<PGMemChunk<T>*>(pointer)->next = freeList;
            freeList = reinterpret_cast<PGMemChunk<T>*>(pointer);

            nbElements--;
        }
    }

    [[nodiscard]] size_t getNbElements() const noexcept { return nbElements; }

    [[nodiscard]] size_t getSize() const noexcept { return size; }

    void destroyAll()
    {
        if (maxAllocatedIndex == 0)
            return;

        // Build a set of free-list pointers for O(1) membership testing.
        std::unordered_set<PGMemChunk<T>*> freeSet;
        PGMemChunk<T>* current = freeList;
        while (current != nullptr)
        {
            freeSet.insert(current);
            current = current->next;
        }

        // Iterate through all allocated indices and destroy objects not in free list
        for (size_t i = 0; i <= maxAllocatedIndex && i < size; ++i)
        {
            PGMemChunk<T>* chunk = getChunk(i);
            if (freeSet.find(chunk) == freeSet.end())
            {
                // This object is still allocated, destroy it
                T* obj = reinterpret_cast<T*>(chunk->element);
                std::destroy_at(obj);
            }
        }

        // Reset pool state - all objects are now destroyed
        nbElements = 0;
        freeList = nullptr;
    }

    T* getSlot(size_t index) const
    {
        return reinterpret_cast<T*>(getChunk(index));
    }

    void advanceCount(size_t n)
    {
        if (n == 0) return;
        const size_t newMax = nbElements + n - 1;
        if (newMax > maxAllocatedIndex)
            maxAllocatedIndex = newMax;
        nbElements += n;
    }

    inline T* getElement(size_t index) const
    {
        if (index >= size)
        {
            return nullptr;
        }

        return reinterpret_cast<T*>(getChunk(index));
    }

protected:

    inline PGMemChunk<T>* getChunk(size_t index) const
    {

        if (N >= 2)
            return &chunkList[index / N][index % N];

        // Block layout (N == 1):
        //   Block 0          : indices [0,   63], size = 64
        //   Block k (k >= 1) : indices [2^(k+5), 2^(k+6) - 1], size = 2^(k+5)
        // For index < 64: block 0, offset = index
        // For index >= 64: n = floor(log2(index)), listPos = n - 5, offset = index - 2^n
        if (index < 64)
            return &chunkList[0][index];

        return nullptr;
    }

private:
    /** Current size of the memory pool */
    size_t size {0};

    /** Current number of elements allocated in the memory pool */
    size_t nbElements{0};

    /** High-water mark - highest index ever allocated */
    size_t maxAllocatedIndex {0};

    /** Pointer to the next free object in the pool */
    PGMemChunk<T>* freeList {nullptr};

    /** PGMemChunk Lists used in the pool (used to free the memory) */
    std::vector<PGMemChunk<T>*> chunkList;
};

}
#endif //BABOMATCHINGENGINE_MEMORY_POOL_H
