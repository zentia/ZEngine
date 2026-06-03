#pragma once

class ObjectPool
{
public:
    void* operator[](int i)
    {
        if (i >= 0 && i < count)
        {
            return list[i]->obj;
        }
        return nullptr;
    }

    void clear()
    {
        freelist = LIST_END;
        count = 0;
        list.clear();
    }

    int Add(void* obj)
    {
        int index = LIST_END;
        if (freelist != LIST_END)
        {
            index = freelist;
            list[index]->obj = obj;
            freelist = list[index]->next;
            list[index]->next = ALLOCED;
        }
        else
        {
            if (count == list.size())
            {
                extend_capacity();
            }
            index = count;
            list[index] = new Slot(ALLOCED, obj);
            count = index + 1;
        }
        return index;
    }
    void* Remove(int index)
    {
        if (index >= 0 && index < count && list[index]->next == ALLOCED)
        {
            void* o = list[index]->obj;
            list[index]->obj = nullptr;
            list[index]->next = freelist;
            freelist = index;
            return o;
        }
        return nullptr;
    }

private:
    void extend_capacity()
    {
        list.resize(list.size() * 2);
    }
    const int LIST_END = -1;
    const int ALLOCED = -2;
    struct Slot
    {
        int next;
        void* obj;

        Slot(int inNext, void* inObj)
        {
            next = inNext;
            obj = inObj;
        }
    };

    std::vector<Slot*> list;
    int freelist = LIST_END;
    int count;
};