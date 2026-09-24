#![allow(unused_crate_dependencies)]

use llzk::{
    builder::{OpBuilder, OpBuilderLike},
    prelude::*,
};
use melior::ir::{
    Block, BlockLike as _, Region, RegionLike as _, Type, ValueLike as _,
    operation::OperationLike as _,
};

type Result<T> = std::result::Result<T, LlzkError>;

const MAIN_STRUCT_NAME: &str = "Main";
const FIELD_NAME: &str = "babybear";

fn main() -> Result<()> {
    let context = LlzkContext::new();
    let location = Location::unknown(&context);
    let main_type = StructType::from_str(&context, MAIN_STRUCT_NAME);
    let field_type = FeltType::with_field(&context, FIELD_NAME);
    let field_mlir_type: Type = field_type.into();

    let mut module_builder = LlzkModuleBuilder::new(&context);
    module_builder
        .with_language("rust-api-validation")
        .with_main(main_type);
    let mut module = module_builder.build();
    let builder = OpBuilder::at_block_begin(&context, module.body());

    dialect::r#struct::def(&builder, location, MAIN_STRUCT_NAME, |builder| {
        dialect::r#struct::member(builder, location, "out", field_type, true, false, true)?;

        let compute_fn =
            dialect::r#struct::helpers::compute_fn(builder, location, main_type, &[], None)?;
        let compute_block = compute_fn.body()?.first_block().unwrap();
        let compute_return = compute_block.terminator().unwrap();
        let self_value = compute_block.first_operation().unwrap().result(0)?;
        {
            let _guard = builder.insertion_guard();
            builder.set_insertion_point(compute_return);
            let one = dialect::felt::constant(
                builder,
                location,
                FeltConstAttribute::new(&context, 1, Some(FIELD_NAME)),
            )?
            .result(0)?;
            dialect::r#struct::writem(builder, location, self_value.into(), "out", one.into())?;
        }

        let constrain_fn =
            dialect::r#struct::helpers::constrain_fn(builder, location, main_type, &[], None)?;
        let constrain_block = constrain_fn.body()?.first_block().unwrap();
        let constrain_return = constrain_block.terminator().unwrap();
        let self_value = constrain_block.argument(0).unwrap();
        {
            let _guard = builder.insertion_guard();
            builder.set_insertion_point(constrain_return);
            let out = dialect::r#struct::readm(
                builder,
                location,
                field_mlir_type,
                self_value.into(),
                "out",
            )?
            .result(0)?;
            let lower = dialect::felt::constant(
                builder,
                location,
                FeltConstAttribute::new(&context, 1, Some(FIELD_NAME)),
            )?
            .result(0)?;
            let upper = dialect::felt::constant(
                builder,
                location,
                FeltConstAttribute::new(&context, 2, Some(FIELD_NAME)),
            )?
            .result(0)?;
            let step = dialect::felt::constant(
                builder,
                location,
                FeltConstAttribute::new(&context, 2_013_265_920, Some(FIELD_NAME)),
            )?
            .result(0)?;

            let before_region = Region::new();
            let before_block = Block::new(&[(field_mlir_type, location)]);
            let before_iv = before_block.argument(0).unwrap();
            let before_builder = OpBuilder::at_block_end(&context, &before_block);
            let continue_loop = dialect::bool::lt(&before_builder, location, before_iv, upper)?;
            before_block.append_operation(melior_dialects::scf::condition(
                continue_loop.result(0)?.into(),
                &[before_iv],
                location,
            ));
            before_region.append_block(before_block);

            let after_region = Region::new();
            let after_block = Block::new(&[(field_mlir_type, location)]);
            let after_iv = after_block.argument(0).unwrap();
            let after_builder = OpBuilder::at_block_end(&context, &after_block);
            dialect::constrain::eq(&after_builder, location, out.into(), after_iv.into());
            let next = dialect::felt::add(&after_builder, location, after_iv, step)?.result(0)?;
            after_block.append_operation(melior_dialects::scf::r#yield(&[next.into()], location));
            after_region.append_block(after_block);

            builder.insert(location, |_, _| {
                melior_dialects::scf::r#while(
                    &[lower],
                    &[field_mlir_type],
                    before_region,
                    after_region,
                    location,
                )
            });
        }

        Ok(())
    })?;

    assert!(
        module.as_operation().verify(),
        "Rust API generated invalid LLZK IR"
    );
    println!("{}", module.as_operation());
    Ok(())
}
