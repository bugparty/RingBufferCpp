//
// Created by fancy on 2022/3/29.
//
#ifndef RINGBUFFERTEST_RINGBUFFER_HPP
#define RINGBUFFERTEST_RINGBUFFER_HPP
#include <iostream>
#include <type_traits>
#include <algorithm>
#include <cstring>
#include <vector>
#pragma
namespace buffers {

    template<typename T, size_t N, bool Overwrite = true>
    class ring_buffer;

    namespace detail {

        /*
         * @param T : type of the element in the buffer
         * @param N : size of the buffer
         * @param Overwrite : if true, the buffer will overwrite the oldest element when the buffer is full.
         * @param C : whether the buffer pointer is const or not
         */
        template<typename T, size_t N, bool C, bool Overwrite>
        class ring_buffer_iterator {
            using buffer_t = typename std::conditional<!C, ring_buffer<T, N, Overwrite>*, ring_buffer<T, N, Overwrite> const*>::type;
        public:
            using self_type = ring_buffer_iterator<T, N, C, Overwrite>;
            using value_type = T;
            using reference = T&;
            using const_reference = T const&;
            using pointer = T*;
            using const_pointer = T const*;
            using size_type = size_t;
            using difference_type = ptrdiff_t;
            using iterator_category = std::forward_iterator_tag;

            ring_buffer_iterator() noexcept = default;

            ring_buffer_iterator(buffer_t source, size_type index, size_type count) noexcept
                    : source_{source},
                      index_{index},
                      count_{count}
            {
            }
            ring_buffer_iterator(ring_buffer_iterator const& ) noexcept = default;
            ring_buffer_iterator& operator=(ring_buffer_iterator const& ) noexcept = default;
            template<bool Z = C, typename std::enable_if<(!Z), int>::type* = nullptr>
            [[nodiscard]] reference operator*() noexcept {
                return (*source_)[index_];
            }
            template<bool Z = C, typename std::enable_if<(Z), int>::type* = nullptr>
            [[nodiscard]] const_reference operator*() const noexcept {
                return (*source_)[index_];
            }
            template<bool Z = C, typename std::enable_if<(!Z), int>::type* = nullptr>
            [[nodiscard]] reference operator->() noexcept {
                return &((*source_)[index_]);
            }
            template<bool Z = C, typename std::enable_if<(Z), int>::type* = nullptr>
            [[nodiscard]] const_reference operator->() const noexcept {
                return &((*source_)[index_]);
            }
            [[nodiscard]] self_type& operator++() noexcept {
                index_ = ++index_ % N;
                ++count_;
                return *this;
            }
            [[nodiscard]] self_type operator++(int) noexcept {
                auto result = *this;
                this->operator*();
                return result;
            }
            [[nodiscard]] size_type index() const noexcept {
                return index_;
            }
            [[nodiscard]] size_type count() const noexcept {
                return count_;
            }
            ~ring_buffer_iterator() = default;
        private:
            buffer_t source_{};
            size_type index_{};
            size_type count_{};
        };

        template<typename T, size_t N, bool C, bool Overwrite>
        bool operator==(ring_buffer_iterator<T,N,C,Overwrite> const& l,
                        ring_buffer_iterator<T,N,C,Overwrite> const& r) noexcept {
            return l.count() == r.count();
        }

        template<typename T, size_t N, bool C, bool Overwrite>
        bool operator!=(ring_buffer_iterator<T,N,C,Overwrite> const& l,
                        ring_buffer_iterator<T,N,C,Overwrite> const& r) noexcept {
            return l.count() != r.count();
        }

    }
#if (__cplusplus < 201703L)
    //backport type_traits from c++17
    template< class T > constexpr bool is_copy_constructible_v = std::is_copy_constructible<T>::value;
    template< class T > constexpr bool is_nothrow_copy_constructible_v = std::is_nothrow_copy_constructible<T>::value;
    template< class T > constexpr bool is_trivially_copyable_v = std::is_trivially_copyable<T>::value;
    template< class T > constexpr bool is_trivially_copy_constructible_v = std::is_trivially_copy_constructible<T>::value;
    template< class T > constexpr bool is_trivially_destructible_v = std::is_trivially_destructible<T>::value;
    template<bool B> using bool_constant = std::integral_constant<bool, B>;
#else
using std::is_copy_constructible_v;
using std::is_nothrow_copy_constructible_v;
using std::is_trivially_copyable_v;
using std::is_trivially_copy_constructible_v;
using std::is_trivially_destructible_v;
using std::bool_constant;
#endif
    template<class T>
    constexpr const T& clamp( const T& v, const T& lo, const T& hi )
    {
        return  v < lo ? lo : v > hi ? hi : v;
    }
    template<typename T, size_t N, bool Overwrite>
    class ring_buffer {
        using self_type = ring_buffer<T, N, Overwrite>;
    public:
        static_assert(N > 0, "ring buffer must have a size greater than zero.");

        using value_type = T;
        using reference = T&;
        using const_reference = T const&;
        using pointer = T*;
        using const_pointer = T const*;
        using size_type = size_t;
        using iterator = detail::ring_buffer_iterator<T, N, false, Overwrite>;
        using const_iterator = detail::ring_buffer_iterator<T, N, true, Overwrite>;

        ring_buffer() noexcept = default;
        ring_buffer(ring_buffer const& rhs) noexcept(is_nothrow_copy_constructible_v<value_type>)
        {
            copy_impl(rhs, bool_constant<is_trivially_copyable_v<T>>{});
        }
        ring_buffer& operator=(ring_buffer const& rhs) noexcept(is_nothrow_copy_constructible_v<value_type>) {
            if(this == &rhs)
                return *this;

            destroy_all(bool_constant<is_trivially_copyable_v<T>>{});
            copy_impl(rhs, bool_constant<is_trivially_copyable_v<T>>{});

            return *this;
        }
        template<typename U>
        void push_back(U&& value) {
            push_back(std::forward<U>(value), bool_constant<Overwrite>{});
        }
        void pop_front() noexcept{
            if(empty())
                return;

            destroy(tail_, bool_constant<is_trivially_destructible_v<value_type>>{});

            --size_;
            tail_ = ++tail_ %N;
        }
        [[nodiscard]] reference back() noexcept { return reinterpret_cast<reference>(elements_[clamp(head_, 0UL, N - 1)]); }
        [[nodiscard]] const_reference back() const noexcept {
            return const_cast<self_type*>(back)->back();
        }
        [[nodiscard]] reference front() noexcept { return reinterpret_cast<reference >(elements_[tail_]); }
        [[nodiscard]] const_reference front() const noexcept {
            return const_cast<self_type*>(this)->front();
        }
        [[nodiscard]] reference operator[](size_type index) noexcept {
            return reinterpret_cast<reference >(elements_[index]);
        }
        [[nodiscard]] const_reference operator[](size_type index) const noexcept {
            return const_cast<self_type *>(this)->operator[](index);
        }
        [[nodiscard]] iterator begin() noexcept { return iterator{this, tail_, 0};}
        [[nodiscard]] iterator end() noexcept { return iterator{this, head_, size_};}
        [[nodiscard]] const_iterator cbegin() const noexcept { return const_iterator{this, tail_, 0};}
        [[nodiscard]] const_iterator cend() const noexcept { return const_iterator{this, head_, size_};}
        [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
        [[nodiscard]] bool full() const noexcept { return size_ == N; }
        [[nodiscard]] size_type size() const noexcept { return size_; }
        [[nodiscard]] size_type capacity() const noexcept { return N; }
       void clear() noexcept {
            destroy_all(bool_constant<is_trivially_destructible_v<value_type>>{});
            size_ = 0;
            head_ = 0;
            tail_ = 0;
        }
        ~ring_buffer() {
            clear();
        };
    private:
        void destroy_all(std::true_type) { }
        void destroy_all(std::false_type) {
            while(!empty()) {
                destroy(tail_, bool_constant<is_trivially_destructible_v<value_type>>{});
                tail_ = ++tail_ % N;
                --size_;
            }
        }
        void copy_impl(self_type const& rhs, std::true_type) {
            std::memcpy(elements_, rhs.elements_, rhs.size_ * sizeof(T));
            size_ = rhs.size_;
            tail_ = rhs.tail_;
            head_ = rhs.head_;
        }
        void copy_impl(self_type const& rhs, std::false_type) {
            tail_ = rhs.tail_;
            head_ = rhs.head_;
            size_ = rhs.size_;
#ifdef __cpp_exceptions
            try {
                for (auto i = 0; i < size_; ++i)
                    // construct value in memory of aligned storage
                    // using inplace operator new
                    new( elements_ + ((tail_ + i) % N)) T(rhs[tail_ + ((tail_ + i) % N)]);
            }catch(...) {
                while(!empty()) {
                    destroy(tail_, bool_constant<std::is_trivially_destructible_v<value_type>>{});
                    tail_ = ++tail_ % N;
                    --size_;
                }
                throw;
            }
#else
            storage_type *p = nullptr;
            for (auto i = 0; i < size_; ++i) {

                // construct value in memory of aligned storage
                // using inplace operator new
                p =reinterpret_cast<storage_type *>(new(elements_ + ((tail_ + i) % N)) T(rhs[tail_ + ((tail_ + i) % N)]));
                if (!p) {
                    break;
                }
            }
            if (!p) {
                while(!empty()) {
                    destroy(tail_, bool_constant<is_trivially_destructible_v<value_type>>{});
                    tail_ = ++tail_ % N;
                    --size_;
                }
            }

#endif
        }
        template<typename U>
        void push_back(U&& value, std::true_type) {
            push_back_impl(std::forward<U>(value));
        }
        template<typename U>
        void push_back(U&& value, std::false_type) {
            if(full() && !Overwrite)
                return;
            push_back_impl(std::forward<U>(value));
        }
        template<typename U>
        void push_back_impl(U&& value) {

            if(full())
                destroy(head_, bool_constant<is_trivially_destructible_v<value_type>>{});

            new(elements_ + head_ ) T{std::forward<U>(value)};
            head_ = ++head_ %N;

            if(full())
                tail_ = ++tail_ % N;

            if(!full())
                ++size_;
        }
        void destroy(size_type index, std::true_type) noexcept { }
        void destroy(size_type index, std::false_type) noexcept {
            reinterpret_cast<pointer >(&elements_[index])->~T();
        }
        using storage_type = typename std::aligned_storage<sizeof(T), alignof(T)>::type;
        storage_type elements_[N]{};
        size_type head_{};
        size_type tail_{};
        size_type size_{};
    };

}
#endif //RINGBUFFERTEST_RINGBUFFER_HPP
