// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <cmath>
#include <cstring>
#include <sstream>

#include "ngraph/coordinate_diff.hpp"
#include "ngraph/node.hpp"
#include "ngraph/runtime/aligned_buffer.hpp"
#include "ngraph/runtime/host_tensor.hpp"
#include "ngraph/runtime/shared_buffer.hpp"
#include "ngraph/type/element_type.hpp"
#include "ngraph/type/element_type_traits.hpp"
#include "ngraph/util.hpp"

namespace ngraph
{
    namespace op
    {
        namespace v0
        {
            /// \brief Class for constants.
            class NGRAPH_API Constant : public Op
            {
            public:
                static constexpr NodeTypeInfo type_info{"Constant", 0};
                const NodeTypeInfo& get_type_info() const override { return type_info; }
                Constant() = default;

                /// \brief Initialize a constant from tensor
                /// \param tensor The tensor with data
                Constant(const std::shared_ptr<runtime::Tensor>& tensor);

                /// \brief Constructs a tensor constant.
                ///
                /// \param type The element type of the tensor constant.
                /// \param shape The shape of the tensor constant.
                /// \param values A vector of literals for initializing the tensor constant. The
                ///               size of values must match the size of the shape.
                template <typename T>
                Constant(const element::Type& type,
                         const Shape& shape,
                         const std::vector<T>& values)
                    : Constant(type, shape)
                {
                    NODE_VALIDATION_CHECK(
                        this,
                        values.size() == 1 || values.size() == shape_size(m_shape),
                        "Did not get the expected number of literals for a constant of shape ",
                        m_shape,
                        " (got ",
                        values.size(),
                        ", expected ",
                        (shape_size(m_shape) == 1 ? "" : "1 or "),
                        shape_size(m_shape),
                        ").");

                    if (values.size() == 1)
                    {
                        write_values(std::vector<T>(shape_size(m_shape), values[0]));
                    }
                    else
                    {
                        write_values(values);
                    }
                    constructor_validate_and_infer_types();
                    m_all_elements_bitwise_identical = are_all_data_elements_bitwise_identical();
                }

                /// \brief Create uninitialized constant
                Constant(const element::Type& type, const Shape& shape);
                /// \brief Constructs a uniform tensor constant.
                ///
                /// \param type The element type of the tensor constant.
                /// \param shape The shape of the tensor constant.
                /// \param value A scalar for initializing the uniform tensor constant. The
                ///               value is broadcast to the specified shape.
                template <class T,
                          class = typename std::enable_if<std::is_fundamental<T>::value>::type>
                Constant(const element::Type& type, const Shape& shape, T value)
                    : Constant(type, shape)
                {
                    using Type_t = element::Type_t;
#if defined(__GNUC__) && !(__GNUC__ == 4 && __GNUC_MINOR__ == 8)
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wswitch"
#pragma GCC diagnostic error "-Wswitch-enum"
#endif
                    switch (type)
                    {
                    case Type_t::boolean: fill_data<Type_t::boolean>(value); break;
                    case Type_t::bf16: fill_data<Type_t::bf16>(value); break;
                    case Type_t::f16: fill_data<Type_t::f16>(value); break;
                    case Type_t::f32: fill_data<Type_t::f32>(value); break;
                    case Type_t::f64: fill_data<Type_t::f64>(value); break;
                    case Type_t::i8: fill_data<Type_t::i8>(value); break;
                    case Type_t::i16: fill_data<Type_t::i16>(value); break;
                    case Type_t::i32: fill_data<Type_t::i32>(value); break;
                    case Type_t::i64: fill_data<Type_t::i64>(value); break;
                    case Type_t::u8: fill_data<Type_t::u8>(value); break;
                    case Type_t::u16: fill_data<Type_t::u16>(value); break;
                    case Type_t::u32: fill_data<Type_t::u32>(value); break;
                    case Type_t::u64: fill_data<Type_t::u64>(value); break;
                    case Type_t::i4:
                    case Type_t::u1:
                    case Type_t::u4:
                    case Type_t::undefined:
                    case Type_t::dynamic: throw std::runtime_error("unsupported type");
                    }
#if defined(__GNUC__) && !(__GNUC__ == 4 && __GNUC_MINOR__ == 8)
#pragma GCC diagnostic pop
#endif
                    constructor_validate_and_infer_types();
                    m_all_elements_bitwise_identical = true;
                }

                /// \brief Constructs a tensor constant
                ///        This constructor is mainly to support deserialization of constants.
                ///
                /// \param type The element type of the tensor constant.
                /// \param shape The shape of the tensor constant.
                /// \param values A list of string values to use as the constant data.
                Constant(const element::Type& type,
                         const Shape& shape,
                         const std::vector<std::string>& values);

                /// \brief Constructs a tensor constant with the supplied data
                ///
                /// \param type The element type of the tensor constant.
                /// \param shape The shape of the tensor constant.
                /// \param data A void* to constant data.
                Constant(const element::Type& type, const Shape& shape, const void* data);

                /// \brief Constructs a tensor constant with the supplied data
                ///
                /// \param type The element type of the tensor constant.
                /// \param shape The shape of the tensor constant.
                /// \param data A pointer to pre-allocated shared data.
                template <typename T>
                Constant(const element::Type& type,
                         const Shape& shape,
                         std::shared_ptr<runtime::SharedBuffer<T>> data)
                    : m_element_type(type)
                    , m_shape(shape)
                {
                    m_data = data;
                    constructor_validate_and_infer_types();
                }

                Constant(const Constant& other);
                Constant& operator=(const Constant&) = delete;

                virtual ~Constant() override;

                void validate_and_infer_types() override
                {
                    infer_element_type();
                    set_output_type(0, m_element_type, m_shape);
                }

                bool visit_attributes(AttributeVisitor& visitor) override;

                bool evaluate(const HostTensorVector& outputs,
                              const HostTensorVector& inputs) const override;
                bool evaluate_lower(const HostTensorVector& outputs) const override;
                bool evaluate_upper(const HostTensorVector& outputs) const override;

                // Don't constant fold a constant; it would make a copy
                bool constant_fold(OutputVector& outputs, const OutputVector& inputs) override
                {
                    return false;
                }

                /// \brief Returns the value of the constant node as a Shape object
                ///        Can only be used on element::i64 nodes and interprets
                ///        negative values as zeros.
                Shape get_shape_val() const;
                /// \brief Returns the value of the constant node as a Strides
                ///        object
                ///        Can only be used on element::i64 nodes and interprets
                ///        negative values as zeros.
                Strides get_strides_val() const;
                /// \brief Returns the value of the constant node as a Coordinate
                ///        object
                ///        Can only be used on element::i64 nodes and interprets
                ///        negative values as zeros.
                Coordinate get_coordinate_val() const;
                /// \brief Returns the value of the constant node as a
                ///        CoordinateDiff object
                ///        Can only be used on element::i64 nodes.
                CoordinateDiff get_coordinate_diff_val() const;
                /// \brief Returns the value of the constant node as an AxisVector
                ///        object
                ///        Can only be used on element::i64 nodes and interprets
                ///        negative values as zeros.
                AxisVector get_axis_vector_val() const;
                /// \brief Returns the value of the constant node as an AxisSet
                ///        object
                ///        Can only be used on element::i64 nodes and interprets
                ///        negative values as zeros.
                ///        Repeated values are allowed.
                AxisSet get_axis_set_val() const;

                /// \brief Update Constant shape. New shape size must equal to the data elements
                /// count
                ///
                /// \param shape The shape of the tensor constant.
                void set_data_shape(const Shape& shape);

                /// \brief Wrapper around constructing a shared_ptr of a Constant
                ///
                /// \param type The element type of the tensor constant.
                /// \param shape The shape of the tensor constant.
                /// \param values A vector of values to use as the constant data.
                template <typename T>
                static std::shared_ptr<op::v0::Constant>
                    create(const element::Type& type, Shape shape, const std::vector<T> values)
                {
                    auto result = std::make_shared<op::v0::Constant>(type, shape, values);
                    result->validate_and_infer_types();
                    return result;
                }

                /// \brief Wrapper around constructing a shared_ptr of a Constant
                ///
                /// \param type The element type of the tensor constant.
                /// \param shape The shape of the tensor constant.
                /// \param values An initializer_list of values to use as the constant data.
                template <typename T>
                static std::shared_ptr<op::v0::Constant>
                    create(const element::Type& type, Shape shape, std::initializer_list<T> values)
                {
                    auto result =
                        std::make_shared<op::v0::Constant>(type, shape, std::vector<T>{values});
                    result->validate_and_infer_types();
                    return result;
                }

                virtual std::shared_ptr<Node>
                    clone_with_new_inputs(const OutputVector& new_args) const override;

                /// \return The initialization literals for the tensor constant.
                std::vector<std::string> get_value_strings() const;

                template <typename T>
                std::vector<T> get_vector() const
                {
                    const T* p = get_data_ptr<T>();
                    if (p == nullptr)
                        throw std::runtime_error("Cannot create vector! Buffer is not allocated.");
                    return std::vector<T>(p, p + shape_size(m_shape));
                }

                /// \brief Return the Constant's value as a vector cast to type T
                ///
                /// \tparam T  Type to which data vector's entries will be cast.
                /// \return    Constant's data vector.
                template <typename T>
                std::vector<T> cast_vector() const
                {
                    auto source_type = get_element_type();
                    std::vector<T> rc;
                    using Type_t = element::Type_t;
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
                    switch (source_type)
                    {
                    case Type_t::boolean: cast_vector<Type_t::boolean>(rc); break;
                    case Type_t::bf16: cast_vector<Type_t::bf16>(rc); break;
                    case Type_t::f16: cast_vector<Type_t::f16>(rc); break;
                    case Type_t::f32: cast_vector<Type_t::f32>(rc); break;
                    case Type_t::f64: cast_vector<Type_t::f64>(rc); break;
                    case Type_t::i8: cast_vector<Type_t::i8>(rc); break;
                    case Type_t::i16: cast_vector<Type_t::i16>(rc); break;
                    case Type_t::i32: cast_vector<Type_t::i32>(rc); break;
                    case Type_t::i64: cast_vector<Type_t::i64>(rc); break;
                    case Type_t::u8: cast_vector<Type_t::u8>(rc); break;
                    case Type_t::u16: cast_vector<Type_t::u16>(rc); break;
                    case Type_t::u32: cast_vector<Type_t::u32>(rc); break;
                    case Type_t::u64: cast_vector<Type_t::u64>(rc); break;
                    default: throw std::runtime_error("unsupported type");
                    }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
                    return rc;
                }

                const void* get_data_ptr() const { return (m_data ? m_data->get_ptr() : nullptr); }
                template <typename T>
                const T* get_data_ptr() const
                {
                    if (sizeof(T) > m_element_type.size() && shape_size(m_shape) > 0)
                    {
                        throw ngraph_error("Buffer over-read");
                    }

                    return static_cast<const T*>(get_data_ptr());
                }

                template <element::Type_t ET>
                const typename element_type_traits<ET>::value_type* get_data_ptr() const
                {
                    NGRAPH_CHECK(ET == get_element_type(),
                                 "get_data_ptr() called for incorrect element type.");
                    return static_cast<const typename element_type_traits<ET>::value_type*>(
                        get_data_ptr());
                }

                bool get_all_data_elements_bitwise_identical() const
                {
                    return m_all_elements_bitwise_identical;
                }
                std::string convert_value_to_string(size_t index) const;

                /**
                 * \brief Allows to avoid buffer allocation on the visit_attributes call
                 */
                void alloc_buffer_on_visit_attributes(bool val)
                {
                    m_alloc_buffer_on_visit_attributes = val;
                }

            protected:
                template <element::Type_t Type, typename OUT_T>
                void cast_vector(std::vector<OUT_T>& output_vector) const
                {
                    // this function is workaround for waring during windows building
                    // build complains for vector creation based on iterators
                    // which point on different type than destination vector::value_type
                    using IN_T = fundamental_type_for<Type>;
                    auto source_vector = get_vector<IN_T>();
                    output_vector.reserve(source_vector.size());

                    std::transform(source_vector.begin(),
                                   source_vector.end(),
                                   std::back_inserter(output_vector),
                                   [](IN_T c) { return static_cast<OUT_T>(c); });
                }

                template <element::Type_t Type,
                          typename T,
                          typename DataStorageType = fundamental_type_for<Type>>
                void fill_data(const T& value)
                {
                    const auto size = shape_size(m_shape);
                    std::fill_n(get_data_ptr_nc<Type>(), size, static_cast<DataStorageType>(value));
                }

                void allocate_buffer();

                void* get_data_ptr_nc() { return (m_data ? m_data->get_ptr() : nullptr); }
                template <element::Type_t ET>
                typename element_type_traits<ET>::value_type* get_data_ptr_nc()
                {
                    NGRAPH_CHECK(ET == get_element_type(),
                                 "get_data_ptr_nc() called for incorrect element type.");
                    return static_cast<typename element_type_traits<ET>::value_type*>(
                        get_data_ptr_nc());
                }

                Constant(const OutputVector& args)
                    : Op(args)
                    , m_shape({})
                {
                }

                virtual void infer_element_type() {}
                template <typename T>
                void write_values(const std::vector<T>& values)
                {
                    write_to_buffer(
                        m_element_type, m_shape, values, get_data_ptr_nc(), shape_size(m_shape));
                }

                template <typename T, typename U>
                static void write_buffer(void* target, const std::vector<U>& source, size_t count)
                {
                    T* p = reinterpret_cast<T*>(target);
                    for (size_t i = 0; i < count; i++)
                    {
                        p[i] = static_cast<T>(source[i]);
                    }
                }

                template <typename T>
                static void write_to_buffer(const element::Type& target_type,
                                            const Shape& /* target_shape */,
                                            const std::vector<T>& source,
                                            void* target,
                                            size_t target_element_count)
                {
                    if (source.size() != target_element_count)
                    {
                        throw std::runtime_error("Constant initializer does not match shape");
                    }
#if defined(__GNUC__) && !(__GNUC__ == 4 && __GNUC_MINOR__ == 8)
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wswitch"
#pragma GCC diagnostic error "-Wswitch-enum"
#endif
                    switch (target_type)
                    {
                    case element::Type_t::boolean:
                        write_buffer<char, T>(target, source, target_element_count);
                        break;
                    case element::Type_t::bf16:
                        write_buffer<bfloat16, T>(target, source, target_element_count);
                        break;
                    case element::Type_t::f16:
                        write_buffer<float16, T>(target, source, target_element_count);
                        break;
                    case element::Type_t::f32:
                        write_buffer<float, T>(target, source, target_element_count);
                        break;
                    case element::Type_t::f64:
                        write_buffer<double, T>(target, source, target_element_count);
                        break;
                    case element::Type_t::i8:
                        write_buffer<int8_t, T>(target, source, target_element_count);
                        break;
                    case element::Type_t::i16:
                        write_buffer<int16_t, T>(target, source, target_element_count);
                        break;
                    case element::Type_t::i32:
                        write_buffer<int32_t, T>(target, source, target_element_count);
                        break;
                    case element::Type_t::i64:
                        write_buffer<int64_t, T>(target, source, target_element_count);
                        break;
                    case element::Type_t::u8:
                        write_buffer<uint8_t, T>(target, source, target_element_count);
                        break;
                    case element::Type_t::u16:
                        write_buffer<uint16_t, T>(target, source, target_element_count);
                        break;
                    case element::Type_t::u32:
                        write_buffer<uint32_t, T>(target, source, target_element_count);
                        break;
                    case element::Type_t::u64:
                        write_buffer<uint64_t, T>(target, source, target_element_count);
                        break;
                    case element::Type_t::i4:
                    case element::Type_t::u1:
                    case element::Type_t::u4:
                    case element::Type_t::undefined:
                    case element::Type_t::dynamic: throw std::runtime_error("unsupported type");
                    }
#if defined(__GNUC__) && !(__GNUC__ == 4 && __GNUC_MINOR__ == 8)
#pragma GCC diagnostic pop
#endif
                }

                bool are_all_data_elements_bitwise_identical() const;
                static constexpr size_t host_alignment() { return 64; }

                element::Type m_element_type;
                Shape m_shape{};
                std::shared_ptr<runtime::AlignedBuffer> m_data;
                bool m_all_elements_bitwise_identical;
                bool m_alloc_buffer_on_visit_attributes = true;
            };
        }
        using v0::Constant;
    }
}
